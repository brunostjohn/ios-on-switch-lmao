"""prelink-bindings.json: every Apple kernel address the prelinked kexts use, bound to our kernel's address.

emit26.py --bindings writes it while names.py's names resolve those addresses; emit26.py --from-bindings reads it in
their place, so that prelink needs only the kernelcache, our unprelinked kernel, the sites and the translator. It holds
for the two inputs whose SHA-256 it records and is refused with any other. Addresses are hex strings, because a 64-bit
kernel address does not survive a JSON reader that parses numbers as doubles.
"""

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

import paths

FORMAT = 1


@dataclass
class Bindings:
    """The export's fields; `table` maps Apple's address to (ours, name)."""

    build: str
    kernelcache_sha256: str
    kernel_sha256: str
    slide: int
    roots: list[str]
    cuts: dict[tuple[str, str], int]
    deferred: dict[str, list[str]]
    table: dict[int, tuple[int, str]] = field(default_factory=dict)

    def resolve(self, apple: int) -> int | None:
        """Return our address bound to Apple's, None for an address the export does not bind."""
        bound = self.table.get(apple)
        return bound[0] if bound else None


class Recorder:
    """Resolves Apple addresses through their names, keeping every binding it hands out."""

    def __init__(self, names: dict[int, str], ours: dict[str, int]) -> None:
        """Bind through `names` (Apple's address -> name) and `ours` (name -> our address)."""
        self._names = names
        self._ours = ours
        self.used: dict[int, tuple[int, str]] = {}

    def resolve(self, apple: int) -> int | None:
        """Return our address of the name Apple's address has, None when either side lacks it."""
        name = self._names.get(apple)
        if not name or name not in self._ours:
            return None
        self.used[apple] = (self._ours[name], name)
        return self._ours[name]


def named(ours: dict[str, int]) -> Recorder:
    """Resolve through names.py's names (the work directory's kernel-names.json), those a kext may link to only.

    Kexts link against the KPI only: a target named outside the symbol sets (or iOS 17's imports) is a naming error,
    so it stays unresolved.
    """
    named_by = json.loads(Path(paths.work('kernel-names.json')).read_text())
    kpi = {line.strip() for line in Path(paths.work('symbolsets26.txt')).read_text().splitlines()}
    # symbol-set entries like __ZN16IOPlatformExpert are prefixes (every member of the class)
    prefixes = tuple(e for e in kpi if re.fullmatch(r'__ZN(\d+[A-Za-z_]\w*?)+', e) and not e.endswith('E'))
    source = named_by['source']

    def exported(apple: str, name: str) -> bool:
        return (
            name in kpi
            or name.startswith((*prefixes, '__ZTV'))
            or name.endswith('gMetaClassE')
            or source.get(apple) in {'vtable', 'got17', 'got17+order', 'symbolicator-class', 'shim'}
        )

    names = {int(apple, 16): name for apple, name in named_by['names'].items() if exported(apple, name)}
    return Recorder(names, ours)


def sha256_file(path: str | Path) -> str:
    """Return the file's SHA-256 as hex."""
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def write(path: str | Path, bindings: Bindings) -> None:
    """Write the export; the same bindings always give the same bytes."""
    doc = {
        'format': FORMAT,
        'build': bindings.build,
        'kernelcache_sha256': bindings.kernelcache_sha256,
        'kernel_sha256': bindings.kernel_sha256,
        'slide': bindings.slide,
        'roots': bindings.roots,
        'cuts': [[component, dependency, sites] for (component, dependency), sites in bindings.cuts.items()],
        'deferred': bindings.deferred,
        'bindings': [
            {'apple': f'{apple:#x}', 'ours': f'{ours:#x}', 'name': name}
            for apple, (ours, name) in sorted(bindings.table.items())
        ],
    }
    Path(path).write_text(json.dumps(doc, indent=1) + '\n')


def load(path: str | Path, kernelcache_sha256: str, kernel_sha256: str) -> Bindings:
    """Read the export, refusing one made for another format, kernelcache or kernel."""
    doc = json.loads(Path(path).read_text())
    if doc.get('format') != FORMAT:
        raise SystemExit(f'{path}: format {doc.get("format")}, this prelink reads format {FORMAT}')
    for key, given in (('kernelcache_sha256', kernelcache_sha256), ('kernel_sha256', kernel_sha256)):
        if doc[key] != given:
            raise SystemExit(f'{path}: made for the {key} {doc[key]}, given {given}')
    table = {}
    for binding in doc['bindings']:
        apple = int(binding['apple'], 16)
        if apple in table:
            raise SystemExit(f'{path}: {apple:#x} bound twice')
        table[apple] = (int(binding['ours'], 16), binding['name'])
    return Bindings(
        build=doc['build'],
        kernelcache_sha256=doc['kernelcache_sha256'],
        kernel_sha256=doc['kernel_sha256'],
        slide=doc['slide'],
        roots=doc['roots'],
        cuts={(component, dependency): sites for component, dependency, sites in doc['cuts']},
        deferred=doc['deferred'],
        table=table,
    )
