"""iOS 27: refuse a deploy whose card holds translated System files this build's trust cache no longer trusts.

The System volume's translated files sit on Data under Switch/pretranslated, placed once by //install:sd
(tools/install/pretranslate.py), and the kernel trusts them through the boot package's os.trustcache. A deploy writes
the boot package but leaves that tree alone: the kernel refuses any change to it. So when a pins change replaces a
translated file's cdhash (edits_24B119.json, for one), a deploy alone would leave the card's old file untrusted, and the
kernel would refuse it at exec. The card's manifest of the tree names each file's cdhashes.
"""

import json
from pathlib import Path

from tools.install import card as cards, renamed
from tools.trustcache import trustcache

MANIFEST = 'pretranslated.json'
SHOWN = 4


def untrusted(manifest: dict, module: bytes) -> list[str]:
    """Return the paths of the manifest's trusted files with a cdhash the module does not hold."""
    trusted = {entry[: trustcache.CDHASH_BYTES].hex() for entry in trustcache.module_entries(module)}
    return [
        f['path']
        for f in manifest.get('files', [])
        if f.get('trust') and any(cdhash.lower() not in trusted for cdhash in f.get('cdhash', []))
    ]


def check(data_root: Path, trust: Path) -> None:
    """Refuse unless every file of the card's translated tree is in `trust`, this build's os.trustcache."""
    manifest = renamed.data_folder(data_root) / MANIFEST
    reinstall = 'reinstall the card with this build (bazel run //install:sd --config=ios27 -- <diskN>)'
    if not manifest.is_file():
        raise cards.RefusedError(f"the card's Data has no {renamed.DATA_FOLDER}/{MANIFEST}: {reinstall}")
    paths = untrusted(json.loads(manifest.read_text()), Path(trust).read_bytes())
    if paths:
        shown = ', '.join(paths[:SHOWN]) + (f' and {len(paths) - SHOWN} more' if len(paths) > SHOWN else '')
        raise cards.RefusedError(
            f"{len(paths)} of the card's translated System files are not in this build's os.trustcache, so the "
            f'kernel would refuse them at exec ({shown}): {reinstall}'
        )
