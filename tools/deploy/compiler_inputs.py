"""Stage the five externally built shader comparison payloads for their matching HW Test binary."""

import hashlib
import mmap
from pathlib import Path

from tools.common import files

STATES = ('3ef3a99e4f382ce1', 'ae6d01e7cac0d964', '17d6ade58fef2152', '357f8355b68acebd', '3f9f7f04b2b91507')


def load(source: Path, package: Path, build: str) -> dict[str, bytes]:
    if build != '24B119':
        raise ValueError('these compiler inputs require the 24B119 development build')
    executable = package / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
    result = {}
    with executable.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as app:
        for state in STATES:
            name = state + '.swab'
            path = source / name
            if path.stat().st_size > 2 << 20:
                raise ValueError(f'{path} exceeds the fixture size limit')
            data = path.read_bytes()
            identity = b'SWAB0001' + (16).to_bytes(4, 'little') + state.encode()
            if not data.startswith(identity) or app.find(hashlib.sha256(data).hexdigest().encode()) < 0:
                raise ValueError(f'{path} does not match the development HW Test in {package}')
            result[name] = data
    return result


def stage(data_root: Path, inputs: dict[str, bytes]) -> None:
    destination = data_root / 'mobile/Media/Switch/benchloop/compiler'
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in inputs.items():
        files.write_bytes(destination / name, data, mode=0o644, sync=True)
