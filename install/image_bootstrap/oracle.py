"""Generate byte-exact golden outputs from tools/install/bootstrap.py and card.py for Rust comparison."""

import base64
import hashlib
import io
import json
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.install import bootstrap, card


@dataclass(frozen=True)
class EntrySpec:
    name: str
    data: bytes = b''
    mode: int = 0o644
    uid: int = 0
    gid: int = 0
    kind: bytes = tarfile.REGTYPE
    link: str = ''


@dataclass(frozen=True)
class CaseSpec:
    name: str
    status: str
    build: str
    version: str
    managed_generation: str | None


def _add(archive: tarfile.TarFile, spec: EntrySpec) -> None:
    info = tarfile.TarInfo(spec.name)
    info.type = spec.kind
    info.mode = spec.mode
    info.uid = spec.uid
    info.gid = spec.gid
    info.linkname = spec.link
    info.size = len(spec.data) if spec.kind == tarfile.REGTYPE else 0
    archive.addfile(info, io.BytesIO(spec.data) if spec.kind == tarfile.REGTYPE else None)


def _bootstrap_case(tmp: Path, case: CaseSpec) -> dict:
    pkg = tmp / case.name / 'pkg'
    data = tmp / case.name / 'data'
    (pkg / 'bootstrap').mkdir(parents=True)
    (pkg / 'overlay/usr/libexec').mkdir(parents=True)
    data.mkdir(parents=True)

    helper = b'switch-bootstrap-helper-bytes'
    (pkg / 'overlay/usr/libexec' / bootstrap.HELPER).write_bytes(helper)

    tar_path = pkg / 'bootstrap' / bootstrap.ARCHIVE
    with tarfile.open(tar_path, 'w') as archive:
        for spec in (
            EntrySpec('.procursus_strapped'),
            EntrySpec('.switch-bootstrap-ready'),
            EntrySpec('Library/dpkg/status', case.status.encode()),
            EntrySpec('usr/bin/tool', b'tool-binary', mode=0o755),
            EntrySpec('usr/bin/suid', b'suid-binary', mode=0o4755),
            EntrySpec('usr/bin/tool-alias', mode=0o777, kind=tarfile.SYMTYPE, link='tool'),
            EntrySpec('usr/bin/hard', mode=0o755, kind=tarfile.LNKTYPE, link='usr/bin/tool'),
            EntrySpec('var/cache/apt/.keep', mode=0o644, uid=501, gid=501),
        ):
            _add(archive, spec)

    tar_bytes = tar_path.read_bytes()
    manifest_in = {
        'schema_version': 1,
        'archive_sha256': hashlib.sha256(tar_bytes).hexdigest(),
        'helper_sha256': hashlib.sha256(helper).hexdigest(),
        'holds': list(bootstrap.HELD),
        'note': 'caf\u00e9 \U0001f600',
    }
    if case.managed_generation is not None:
        manifest_in['managed_generation'] = case.managed_generation
    manifest_in_bytes = json.dumps(manifest_in).encode()
    (pkg / 'bootstrap' / bootstrap.MANIFEST).write_bytes(manifest_in_bytes)

    logs = []
    bootstrap.stage(data, pkg, {'build': case.build, 'version': case.version}, logs.append)

    installed = data / bootstrap.DIRECTORY / case.build
    pending = installed / 'migration.pending'
    return {
        'name': case.name,
        'build': case.build,
        'version': case.version,
        'manifest_in_b64': base64.b64encode(manifest_in_bytes).decode('ascii'),
        'archive_b64': base64.b64encode(tar_bytes).decode('ascii'),
        'helper_b64': base64.b64encode(helper).decode('ascii'),
        'expected_manifest_b64': base64.b64encode((installed / bootstrap.MANIFEST).read_bytes()).decode('ascii'),
        'expected_metadata_b64': base64.b64encode((installed / bootstrap.METADATA).read_bytes()).decode('ascii'),
        'expected_status_b64': base64.b64encode((installed / 'root/Library/dpkg/status').read_bytes()).decode('ascii'),
        'expected_pending_b64': base64.b64encode(pending.read_bytes()).decode('ascii') if pending.exists() else None,
    }


def main() -> None:
    out = Path(sys.argv[1])
    status_existing_fw = (
        'Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n'
        'Description: Sileo\n Line two\n .\n Line four\n\n'
        'Package: ellekit\nStatus: install ok installed\nVersion: 1.1\n\n'
        'Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n'
        'Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n\n'
        'Package: firmware\nStatus: install ok installed\nVersion: 15.0\n'
    )
    status_no_fw = (
        'Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n\n'
        'Package: ellekit\nStatus: hold ok installed\nVersion: 1.1\n\n'
        'Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n'
        'Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n'
    )

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        cases = [
            _bootstrap_case(tmp, CaseSpec('ios17_existing_fw', status_existing_fw, '21H461', '17.7.11', 'a' * 64)),
            _bootstrap_case(tmp, CaseSpec('ios27_insert_fw', status_no_fw, '24B119', '27.2', None)),
        ]

        overlay_dir = tmp / 'overlay_digest'
        (overlay_dir / 'usr/bin').mkdir(parents=True)
        (overlay_dir / 'etc').mkdir(parents=True)
        tool = overlay_dir / 'usr/bin/tool'
        tool.write_bytes(b'bin')
        tool.chmod(0o555)
        cfg = overlay_dir / 'etc/conf'
        cfg.write_bytes(b'cfg')
        cfg.chmod(0o444)
        (overlay_dir / 'etc/._conf').write_bytes(b'ignored')
        overlay_digest = card.tree_digest(overlay_dir)

    out.write_text(json.dumps({'bootstrap': cases, 'overlay_digest': overlay_digest}, indent=2) + '\n')


if __name__ == '__main__':
    main()
