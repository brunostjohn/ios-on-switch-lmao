"""What LaunchServices registers for the overlay's system apps, digested: a migration is worth only a change to it.

Per bundle (each .app and the .appex bundles inside it): its path, its Info.plist's contents, its localized
InfoPlist strings, and its executable's entitlements and code-signing identifier and team. The executable's code, its
cdhash and Mach-O UUID, and every other resource are left out: a rebuilt app at the same path launches on the record
the last migration made.
"""

import hashlib
import json
import plistlib
import struct
from pathlib import Path

from tools.common import macho

SUPERBLOB = 0xFADE0CC0
CODE_DIRECTORY = 0xFADE0C02
# The embedded signature's entitlements blobs: the XML plist (slot 5) and its DER form (slot 7).
ENTITLEMENT_SLOTS = (5, 7)
TEAM_ID_VERSION = 0x20200
BUNDLE_SUFFIXES = ('.app', '.appex')
LOCALIZED_INFO = ('InfoPlist.strings', 'InfoPlist.loctable')


def _canonical(value: object) -> object:
    if isinstance(value, dict):
        return {key: _canonical(item) for key, item in sorted(value.items())}
    if isinstance(value, list):
        return [_canonical(item) for item in value]
    if isinstance(value, bytes):
        return {'bytes': value.hex()}
    if not isinstance(value, str | int | float | bool) and value is not None:
        return {'value': str(value)}
    return value


def _info(bundle: Path) -> dict:
    try:
        return plistlib.loads((bundle / 'Info.plist').read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError):
        return {}


def _signature_blobs(image: bytes) -> dict[int, bytes]:
    """Return the embedded signature's blobs by slot ({} when the image has none)."""
    try:
        parsed = macho.macho(image, subtypes=None)
    except ValueError:
        return {}
    for at, _ in macho.commands_of(parsed, macho.LC_CODE_SIGNATURE)[:1]:
        offset, size = struct.unpack_from('<II', image, at + 8)
        if offset + 12 > len(image) or offset + size > len(image):
            return {}
        magic, _, count = struct.unpack_from('>III', image, offset)
        if magic != SUPERBLOB:
            return {}
        blobs = {}
        for index in range(count):
            slot, start = struct.unpack_from('>II', image, offset + 12 + 8 * index)
            _, length = struct.unpack_from('>II', image, offset + start)
            blobs[slot] = image[offset + start : offset + start + length]
        return blobs
    return {}


def _c_string(blob: bytes, at: int) -> str:
    end = blob.find(b'\0', at)
    return blob[at : end if end >= 0 else len(blob)].decode(errors='replace')


def signing(executable: Path) -> dict:
    """Return the executable's entitlements and CodeDirectory identifier and team, or its digest when unsigned."""
    try:
        image = executable.read_bytes()
    except OSError:
        return {'executable': 'missing'}
    blobs = _signature_blobs(image)
    directory = blobs.get(0, b'')
    if directory[:4] != struct.pack('>I', CODE_DIRECTORY):
        return {'unsigned': hashlib.sha256(image).hexdigest()}
    version, _, _, identifier_at = struct.unpack_from('>IIII', directory, 8)
    team = ''
    if version >= TEAM_ID_VERSION and len(directory) >= 52:
        team_at = struct.unpack_from('>I', directory, 48)[0]
        team = _c_string(directory, team_at) if team_at else ''
    return {
        'identifier': _c_string(directory, identifier_at),
        'team': team,
        'entitlements': {str(slot): blobs[slot][8:].hex() for slot in ENTITLEMENT_SLOTS if slot in blobs},
    }


def bundle_record(bundle: Path, root: Path) -> dict:
    info = _info(bundle)
    localized = {
        path.relative_to(bundle).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for name in LOCALIZED_INFO
        for path in sorted(bundle.glob(f'*.lproj/{name}'))
        if path.is_file()
    }
    executable = info.get('CFBundleExecutable')
    return {
        'path': bundle.relative_to(root).as_posix(),
        'info': _canonical(info),
        'localized': localized,
        'signing': signing(bundle / executable) if isinstance(executable, str) else {},
    }


def bundles(root: Path) -> list[Path]:
    """Every .app directly in `root` and every .app or .appex inside one, outermost first."""
    found = []
    for app in sorted(path for path in root.glob('*.app') if path.is_dir()):
        found.append(app)
        found += sorted(path for path in app.rglob('*') if path.is_dir() and path.suffix in BUNDLE_SUFFIXES)
    return found


def digest(root: Path) -> str | None:
    """Return the digest of what LaunchServices registers for the apps in `root`, or None without a `root`."""
    root = Path(root)
    if not root.is_dir():
        return None
    records = [bundle_record(bundle, root) for bundle in bundles(root)]
    return hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()
