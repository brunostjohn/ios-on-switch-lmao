#!/usr/bin/env python3
"""Write the raw GM20B path's stack key (the old build script's raw_stack()).

usage: raw_stack.py --nvk <iOS NVK dylib> --drivers <tree/drivers> --m2v-root <third_party/m2v> --m2v <file>...
                    --source <bridge or frontend file>... --raw <drivers/TegraGM20BMetal/raw> --out <file>

The stack key is a hash of what the raw pack's contents depend on, known before the plugin is compiled, which is
compiled in (-DTEGRA_RAW_STACK) and stamped into the pack, so the device refuses a pack of another build. The NVK dylib
(its context init, NAK's programs and register values), m2v (every source file of the crates the bridge links, by its
path below the m2v tree, in path order), every bridge source and header the rule names (the binding rules and each
record form's compile target), the frontend (the journal's state keys) and the raw runtime and pack generator (every
.h/.c/.cpp/.mm/.py/.inc/.metal below raw/, in path order), the last three each by its file name and bytes. Writes the
first 16 hex digits.
"""

import argparse
import hashlib
from pathlib import Path

SUFFIXES = ('.h', '.c', '.cpp', '.mm', '.py', '.inc', '.metal')


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('nvk', 'drivers', 'm2v-root', 'raw', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--m2v', action='append', default=[], help='an m2v source file (below --m2v-root)')
    parser.add_argument('--source', action='append', default=[], help='a bridge or frontend file')
    args = parser.parse_args()
    drivers, raw, m2v_root = Path(args.drivers), Path(args.raw), Path(args.m2v_root)
    m2v = sorted(Path(path).relative_to(m2v_root).as_posix() for path in args.m2v)
    sources = sorted((Path(path) for path in args.source), key=lambda path: path.name)
    names = [path.name for path in sources]
    if len(set(names)) != len(names) or 'NativeMetalBridge.rs' not in names:
        raise SystemExit('raw_stack.py: --source names each bridge file once, NativeMetalBridge.rs among them')
    raw_files = sorted(p for p in raw.rglob('*') if p.is_file() and p.suffix in SUFFIXES)
    digest = hashlib.sha256(Path(args.nvk).read_bytes())
    for relative in m2v:
        digest.update(b'm2v/' + relative.encode() + b'\0' + (m2v_root / relative).read_bytes())
    for path in [*sources, *raw_files]:
        digest.update(str(path.name).encode() + b'\0' + path.read_bytes())
    Path(args.out).write_text(digest.hexdigest()[:16] + '\n')


if __name__ == '__main__':
    main()
