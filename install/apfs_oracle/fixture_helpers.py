"""Fixture generator and mounted-volume verifiers for oracle_fixture_test.sh."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import plistlib
import struct
import tarfile
from pathlib import Path

HELD_PACKAGES = ['org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl']
SAMPLE_DPKG_STATUS = (
    'Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n\n'
    'Package: ellekit\nStatus: install ok installed\nVersion: 1.1\n\n'
    'Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n'
    'Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n\n'
    'Package: firmware\nStatus: install ok installed\nVersion: 15.0\n'
)
UF_COMPRESSED = 0x20
UF_HIDDEN = 0x8000


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_delta(sys_mount: Path, delta_dir: Path) -> None:
    replacements = [
        ('/private/var/db/plain', b'translated plain var file\n', []),
        ('/private/var/db/compressed', b'translated uncompressed var file\n', []),
        ('/private/var/db/hl1', b'translated hardlinked var file\n', []),
        ('/private/var/db/hl2', b'translated hardlinked var file\n', []),
        ('/private/var/staged_system_apps/AppleApp.app/AppleApp', b'translated staged app binary\n', ['44' * 20]),
        ('/usr/libexec/tool', b'translated system tool binary\n', ['11' * 20]),
        ('/usr/libexec/linked_a', b'translated system hardlink binary\n', ['22' * 20]),
        ('/usr/sbin/linked_b', b'translated system hardlink binary\n', ['22' * 20]),
    ]
    files = []
    for vpath, out_bytes, cdhash in replacements:
        in_bytes = (sys_mount / vpath.lstrip('/')).read_bytes()
        rel = 'files' + vpath
        dest = delta_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(out_bytes)
        files.append(
            {
                'path': vpath,
                'file': rel,
                'sha256_in': _sha256(in_bytes),
                'sha256_out': _sha256(out_bytes),
                'cdhash': cdhash,
            }
        )
    (delta_dir / 'os.manifest.json').write_text(
        json.dumps({'role': 'os', 'translator': 'oracle-fixture', 'files': files}, indent=1) + '\n',
        encoding='utf-8',
    )
    (delta_dir / 'os.cdhash-pairs').write_text(f'{"33" * 20} {"44" * 20}\n', encoding='utf-8')


def _write_overlay(overlay_dir: Path) -> bytes:
    helper_bytes = b'#!/bin/sh\necho switch-bootstrap-oracle\n'
    helper = overlay_dir / 'usr/libexec/switch-bootstrap'
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_bytes(helper_bytes)
    os.chmod(helper, 0o755)

    (overlay_dir / 'usr/bin').mkdir(parents=True, exist_ok=True)
    os.symlink('../libexec/switch-bootstrap', overlay_dir / 'usr/bin/overlay-link')

    conf = overlay_dir / 'etc/switch.conf'
    conf.parent.mkdir(parents=True, exist_ok=True)
    conf.write_bytes(b'oracle=1\n')
    os.chmod(conf, 0o444)
    (overlay_dir / 'etc/._ignored').write_bytes(b'apple-double-junk')

    demo_info = plistlib.dumps(
        {
            'CFBundleIdentifier': 'com.example.demo',
            'CFBundleExecutable': 'Demo',
            'CFBundleVersion': '8',
        }
    )
    demo_dir = overlay_dir / 'usr/share/Switch/Applications/Demo.app'
    (demo_dir / '_CodeSignature').mkdir(parents=True, exist_ok=True)
    (demo_dir / 'Info.plist').write_bytes(demo_info)
    (demo_dir / 'Demo').write_bytes(b'demo-staged-app-binary\n')
    os.chmod(demo_dir / 'Demo', 0o555)
    (demo_dir / '_CodeSignature/CodeResources').write_bytes(b'demo-code-resources\n')
    ph = overlay_dir / 'System/Library/AppPlaceholders/Demo.app/Info.plist'
    ph.parent.mkdir(parents=True, exist_ok=True)
    ph.write_bytes(demo_info)
    sig = overlay_dir / 'System/Library/AppSignatures/com.example.demo'
    sig.parent.mkdir(parents=True, exist_ok=True)
    sig.write_bytes(b'detached-signature-demo\n')
    return helper_bytes


def _write_bootstrap(bootstrap_dir: Path, helper_bytes: bytes) -> None:
    bootstrap_dir.mkdir(parents=True, exist_ok=True)
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode='w') as tf:
        members: list[tuple[str, bytes, int, int, int]] = [
            ('.procursus_strapped', b'', 0o644, 0, 0),
            ('.switch-bootstrap-ready', b'', 0o644, 0, 0),
            ('Library/dpkg/status', SAMPLE_DPKG_STATUS.encode(), 0o644, 0, 0),
            ('usr/bin/jbtool', b'jbtool-payload\n', 0o755, 0, 0),
            ('usr/bin/suid-helper', b'suid-payload\n', 0o4755, 0, 0),
            ('var/cache/apt/.keep', b'', 0o644, 501, 501),
        ]
        for name, data, mode, uid, gid in members:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            ti.uid = uid
            ti.gid = gid
            ti.mtime = 0
            tf.addfile(ti, io.BytesIO(data))

        hl = tarfile.TarInfo('usr/bin/jbtool-hl')
        hl.type = tarfile.LNKTYPE
        hl.linkname = 'usr/bin/jbtool'
        hl.mode = 0o755
        tf.addfile(hl)

        sym = tarfile.TarInfo('usr/bin/jbtool-sym')
        sym.type = tarfile.SYMTYPE
        sym.linkname = 'jbtool'
        sym.mode = 0o777
        tf.addfile(sym)

    tar_bytes = tar_buf.getvalue()
    (bootstrap_dir / 'payload.tar').write_bytes(tar_bytes)
    manifest = {
        'schema_version': 1,
        'archive_sha256': _sha256(tar_bytes),
        'helper_sha256': _sha256(helper_bytes),
        'holds': HELD_PACKAGES,
        'managed_generation': 'a' * 64,
    }
    (bootstrap_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def _cmd_prepare_inputs(args: argparse.Namespace) -> int:
    _write_delta(Path(args.sys_mount), Path(args.delta_dir))
    helper_bytes = _write_overlay(Path(args.overlay_dir))
    _write_bootstrap(Path(args.bootstrap_dir), helper_bytes)
    seed_file = Path(args.seed_dir) / 'mobile/Library/Caches/com.apple.metal/seed.metallib'
    seed_file.parent.mkdir(parents=True, exist_ok=True)
    seed_file.write_bytes(b'MTLB-oracle-shader-seed\n')
    return 0


def _cmd_prepare_cryptex_delta(args: argparse.Namespace) -> int:
    c_mount = Path(args.cryptex_mount)
    out_dir = Path(args.delta_dir)
    app_out = b'rebuilt-cryptex-app-' + b'A' * 36000 + b'\n'
    rep_out = b'uncompressed replacement for words-rep\n'
    items = [
        ('/bin/app', app_out),
        ('/c/words-rep', rep_out),
    ]
    files = []
    for vpath, payload in items:
        in_bytes = (c_mount / vpath.lstrip('/')).read_bytes()
        rel = 'files' + vpath
        dest = out_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(payload)
        files.append(
            {
                'path': vpath,
                'file': rel,
                'sha256_in': _sha256(in_bytes),
                'sha256_out': _sha256(payload),
                'cdhash': [],
            }
        )
    (out_dir / 'cryptex.manifest.json').write_text(
        json.dumps({'role': 'app_cryptex', 'translator': 'oracle-fixture', 'files': files}, indent=1) + '\n',
        encoding='utf-8',
    )
    return 0


def _cmd_verify_cryptex_mount(args: argparse.Namespace) -> int:
    mp = Path(args.mount)
    delta_dir = Path(args.delta_dir)
    want_app = (delta_dir / 'files/bin/app').read_bytes()
    want_rep = (delta_dir / 'files/c/words-rep').read_bytes()
    want_kept = Path(args.words_file).read_bytes()

    st_app = os.lstat(mp / 'bin/app')
    if (st_app.st_uid, st_app.st_gid, st_app.st_mode & 0o7777) != (0, 0, 0o4755):
        raise SystemExit(f'cryptex /bin/app stat mismatch: {st_app}')
    if (mp / 'bin/app').read_bytes() != want_app:
        raise SystemExit('cryptex /bin/app content mismatch')

    st_rep = os.lstat(mp / 'c/words-rep')
    if st_rep.st_flags & UF_COMPRESSED:
        raise SystemExit('cryptex /c/words-rep still compressed after replacement')
    if (mp / 'c/words-rep').read_bytes() != want_rep:
        raise SystemExit('cryptex /c/words-rep content mismatch')

    st_kept = os.lstat(mp / 'c/words-kept')
    if (st_kept.st_flags & UF_COMPRESSED) == 0:
        raise SystemExit('cryptex /c/words-kept lost UF_COMPRESSED')
    if (mp / 'c/words-kept').read_bytes() != want_kept:
        raise SystemExit('cryptex /c/words-kept content mismatch')

    hl1 = os.lstat(mp / 'hl/kept1')
    hl2 = os.lstat(mp / 'hl/kept2')
    if hl1.st_ino != hl2.st_ino or hl1.st_nlink != 2 or (hl1.st_uid, hl1.st_gid) != (501, 20):
        raise SystemExit(f'cryptex hard link mismatch: {hl1}, {hl2}')
    if (hl1.st_flags & UF_HIDDEN) == 0:
        raise SystemExit('cryptex /hl/kept1 lost UF_HIDDEN')
    if os.readlink(mp / 'x/sym') != '../bin/app':
        raise SystemExit('cryptex /x/sym target mismatch')

    print('mounted rebuilt cryptex verified: owners, setuid, compression, hard links, symlink, SHA-256 OK')
    return 0


def _cmd_check_volume_plist(args: argparse.Namespace) -> int:
    with open(args.plist, 'rb') as f:
        info = plistlib.load(f)
    got_uuid = str(info.get('VolumeUUID', '')).upper()
    got_name = str(info.get('VolumeName', ''))
    if got_uuid != args.uuid.upper():
        raise SystemExit(f'VolumeUUID {got_uuid!r} != {args.uuid.upper()!r}')
    if got_name != args.name:
        raise SystemExit(f'VolumeName {got_name!r} != {args.name!r}')
    if args.require_sealed and info.get('Sealed') not in (True, 'Yes'):
        raise SystemExit(f'expected Sealed volume, got Sealed={info.get("Sealed")!r}')
    print(f'diskutil plist OK: Name={got_name} UUID={got_uuid} Sealed={info.get("Sealed")}')
    return 0


def _parse_index_tsv(path: Path) -> tuple[dict[str, tuple[int, int]], list[int], list[tuple[str, str]]]:
    idx_files: dict[str, tuple[int, int]] = {}
    idx_dirs: list[int] = []
    idx_origs: list[tuple[str, str]] = []
    for line in path.read_text(encoding='utf-8').splitlines():
        parts = line.split('\t')
        if parts[0] == 'file':
            idx_files[parts[1]] = (int(parts[2]), int(parts[3]))
        elif parts[0] == 'dir':
            idx_dirs.append(int(parts[1]))
        elif parts[0] == 'orig':
            idx_origs.append((parts[1], parts[2]))
    return idx_files, idx_dirs, idx_origs


def _verify_index_inodes(sys_root: Path, pt_root: Path, idx_files: dict[str, tuple[int, int]]) -> None:
    by_sys: dict[int, list[str]] = {}
    for path, (sys_id, _) in idx_files.items():
        real_sys = os.lstat(sys_root / path.lstrip('/')).st_ino
        if real_sys != sys_id:
            raise SystemExit(f'{path}: system inode {sys_id} != mounted lstat {real_sys}')
        by_sys.setdefault(sys_id, []).append(path)

    for sys_id, paths in by_sys.items():
        want_data = idx_files[paths[0]][1]
        matched = 0
        for path in paths:
            placed = pt_root / path.lstrip('/')
            if placed.exists():
                real_data = os.lstat(placed).st_ino
                if real_data != want_data:
                    raise SystemExit(f'{placed}: data inode {want_data} != mounted lstat {real_data}')
                matched += 1
        if matched == 0:
            raise SystemExit(f'system inode {sys_id}: no placed file mounted under {pt_root}')


def _cmd_verify_mounted_index(args: argparse.Namespace) -> int:
    sys_root = Path(args.sys_mount)
    data_root = Path(args.data_mount)
    pt_root = data_root / 'Switch/pretranslated'

    idx_files, idx_dirs, idx_origs = _parse_index_tsv(Path(args.index_tsv))
    raw_idx = (pt_root / '.index').read_bytes()
    magic, ver, fcount, dcount, _, res = struct.unpack_from('<4sIIIII', raw_idx)
    if (magic, ver, fcount, dcount, res) != (b'SWOV', 2, len(idx_files), len(idx_dirs), 0):
        raise SystemExit(f'unexpected .index header: {(magic, ver, fcount, dcount, res)}')

    actual_dirs = sorted(os.lstat(d).st_ino for d in [pt_root, *(p for p in pt_root.rglob('*') if p.is_dir())])
    if sorted(idx_dirs) != actual_dirs:
        raise SystemExit(f'mounted dir inodes mismatch: index={sorted(idx_dirs)} actual={actual_dirs}')

    _verify_index_inodes(sys_root, pt_root, idx_files)
    if idx_origs != [('33' * 20, '44' * 20)]:
        raise SystemExit(f'unexpected .originals pairs: {idx_origs}')

    _verify_mounted_features(data_root)
    print(
        f'mounted .index verified: {len(idx_files)} files, {len(idx_dirs)} dirs, {len(idx_origs)} originals, '
        'bootstrap + staged app + overlay + shader seed OK'
    )
    return 0


def _verify_mounted_features(data_root: Path) -> None:
    if os.readlink(data_root / 'jb') != '/private/var/Switch/bootstrap/24B119/root':
        raise SystemExit(f'bad /jb symlink: {os.readlink(data_root / "jb")}')
    dpkg = (data_root / 'Switch/bootstrap/24B119/root/Library/dpkg/status').read_text(encoding='utf-8')
    if 'Version: 27.2' not in dpkg or 'Status: hold ok installed' not in dpkg:
        raise SystemExit('dpkg status was not rewritten for 27.2 with holds')
    if (data_root / 'Switch/bootstrap/24B119/migration.pending').read_text(encoding='utf-8') != 'a' * 64 + '\n':
        raise SystemExit('migration.pending content mismatch')
    if (data_root / 'Switch/overlay/etc/._ignored').exists():
        raise SystemExit('AppleDouble file leaked into Switch/overlay')
    st_app = os.lstat(data_root / 'staged_system_apps/Demo.app/Demo')
    if (st_app.st_uid, st_app.st_gid) != (501, 501):
        raise SystemExit(f'staged Demo.app owner {(st_app.st_uid, st_app.st_gid)} != (501, 501)')
    st_seed = os.lstat(data_root / 'mobile/Library/Caches/com.apple.metal/seed.metallib')
    if (st_seed.st_uid, st_seed.st_gid, st_seed.st_mode & 0o7777) != (0, 0, 0o644):
        raise SystemExit(f'shader seed stat mismatch: {st_seed}')


def _cmd_mutate_dump_owner(args: argparse.Namespace) -> int:
    data = json.loads(Path(args.src).read_text(encoding='utf-8'))
    entry = data['entries'][args.path]
    entry['uid'] = 4242 if entry['uid'] != 4242 else 4243
    Path(args.dst).write_text(json.dumps(data, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='cmd', required=True)

    p1 = sub.add_parser('prepare-inputs')
    p1.add_argument('--sys-mount', required=True)
    p1.add_argument('--delta-dir', required=True)
    p1.add_argument('--overlay-dir', required=True)
    p1.add_argument('--bootstrap-dir', required=True)
    p1.add_argument('--seed-dir', required=True)
    p1.set_defaults(func=_cmd_prepare_inputs)

    p2 = sub.add_parser('prepare-cryptex-delta')
    p2.add_argument('--cryptex-mount', required=True)
    p2.add_argument('--delta-dir', required=True)
    p2.set_defaults(func=_cmd_prepare_cryptex_delta)

    p3 = sub.add_parser('verify-cryptex-mount')
    p3.add_argument('--mount', required=True)
    p3.add_argument('--delta-dir', required=True)
    p3.add_argument('--words-file', required=True)
    p3.set_defaults(func=_cmd_verify_cryptex_mount)

    p4 = sub.add_parser('check-volume-plist')
    p4.add_argument('--plist', required=True)
    p4.add_argument('--uuid', required=True)
    p4.add_argument('--name', required=True)
    p4.add_argument('--require-sealed', action='store_true')
    p4.set_defaults(func=_cmd_check_volume_plist)

    p5 = sub.add_parser('verify-mounted-index')
    p5.add_argument('--sys-mount', required=True)
    p5.add_argument('--data-mount', required=True)
    p5.add_argument('--index-tsv', required=True)
    p5.set_defaults(func=_cmd_verify_mounted_index)

    p6 = sub.add_parser('mutate-dump-owner')
    p6.add_argument('--src', required=True)
    p6.add_argument('--dst', required=True)
    p6.add_argument('--path', required=True)
    p6.set_defaults(func=_cmd_mutate_dump_owner)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == '__main__':
    raise SystemExit(main())
