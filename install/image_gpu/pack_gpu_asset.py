#!/usr/bin/env python3
"""Pack //gpu:base and //kernel:gpu_kext_digest into a deterministic format-2 GPU release tar.

usage: pack_gpu_asset.py --base <dir> --kext-digest <file> --out <tar>
"""

import argparse
import hashlib
import io
import tarfile
from pathlib import Path, PurePosixPath

HEADER = '# gpu stage 2 tree'
KEXT_PREFIX = '# kext '
TREE_MANIFEST = 'gpu.stage'
REQUIRED_PLUGIN_PATH = 'ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal'


def check_rel_path(path: str) -> str:
    parts = PurePosixPath(path).parts
    if not parts or path.startswith('/') or '\0' in path or any(p in ('', '.', '..') for p in parts):
        raise ValueError(f'unsafe relative path: {path!r}')
    return path


def _parse_file_record(base: Path, manifest_path: Path, line_no: int, fields: list[str]) -> tuple[str, int, bytes]:
    if len(fields) != 6:
        raise ValueError(f'{manifest_path}:{line_no}: wrong file record field count ({len(fields)})')
    _, path, want_sha, size_str, mode_str, source = fields
    check_rel_path(path)
    if path != source or not path.startswith('ngpu/'):
        raise ValueError(f'{manifest_path}:{line_no}: invalid tree file record {path!r} -> {source!r}')
    want_size = int(size_str)
    mode = int(mode_str, 8)
    if not 0 <= mode <= 0o7777:
        raise ValueError(f'{manifest_path}:{line_no}: invalid mode {mode_str!r}')
    src = base / path
    # Bazel's darwin-sandbox materializes TreeArtifact inputs as absolute symlinks into the execroot.
    if (src.is_symlink() and not src.readlink().is_absolute()) or not src.is_file():
        raise ValueError(f'{manifest_path}:{line_no}: missing regular file {src}')
    raw = src.read_bytes()
    if len(raw) != want_size:
        raise ValueError(f'{path}: size {len(raw)} != {want_size}')
    got_sha = hashlib.sha256(raw).hexdigest()
    if got_sha != want_sha:
        raise ValueError(f'{path}: sha256 {got_sha} != {want_sha}')
    return path, mode, raw


def _parse_manifest_body(
    base: Path, manifest_path: Path, lines: list[str]
) -> tuple[dict[str, tuple[int, bytes]], list[str]]:
    files: dict[str, tuple[int, bytes]] = {}
    body: list[str] = []
    for line_no, line in enumerate(lines, start=2):
        if line.startswith(KEXT_PREFIX):
            raise ValueError(f'{manifest_path}:{line_no}: unexpected existing {KEXT_PREFIX!r} line')
        fields = line.split('\t')
        if fields[0] == 'file':
            path, mode, raw = _parse_file_record(base, manifest_path, line_no, fields)
            if path in files:
                raise ValueError(f'{manifest_path}:{line_no}: duplicate file {path!r}')
            files[path] = (mode, raw)
            body.append(line)
        elif fields[0] == 'pref':
            if len(fields) != 5:
                raise ValueError(f'{manifest_path}:{line_no}: wrong pref record field count ({len(fields)})')
            body.append(line)
        else:
            raise ValueError(f'{manifest_path}:{line_no}: unknown record {fields[0]!r}')
    return files, body


def _write_deterministic_tar(entries: dict[str, tuple[int, bytes]]) -> bytes:
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(entries):
            mode, raw = entries[name]
            info = tarfile.TarInfo(name)
            info.size = len(raw)
            info.mode = mode
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ''
            info.gname = ''
            tar.addfile(info, io.BytesIO(raw))
    return out.getvalue()


def build_tar(base: Path, kext_digest: str) -> bytes:
    digest = kext_digest.strip()
    if not digest or any(ch.isspace() for ch in digest):
        raise ValueError(f'invalid kext digest: {kext_digest!r}')

    manifest_path = base / TREE_MANIFEST
    lines = manifest_path.read_text(encoding='utf-8').splitlines()
    if not lines or lines[0] != HEADER:
        raise ValueError(f'{manifest_path}: expected header {HEADER!r}')

    files, body = _parse_manifest_body(base, manifest_path, lines[1:])
    if REQUIRED_PLUGIN_PATH not in files:
        raise ValueError(f'{manifest_path}: missing required plugin {REQUIRED_PLUGIN_PATH}')

    manifest_bytes = ('\n'.join([HEADER, KEXT_PREFIX + digest, *body]) + '\n').encode('utf-8')
    return _write_deterministic_tar({TREE_MANIFEST: (0o644, manifest_bytes), **files})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--kext-digest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    tar_bytes = build_tar(args.base, args.kext_digest.read_text(encoding='utf-8'))
    args.out.write_bytes(tar_bytes)


if __name__ == '__main__':
    main()
