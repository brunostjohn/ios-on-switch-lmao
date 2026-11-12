"""The Core Animation states this device's QuartzCore asks for in place of the offline compilation manifest's.

ca_asked.py <qc-<build>.json> <stage-tegra-gm20b-metal.py> <out>. Apple's manifest names its states for extended-colour
devices; this device's QuartzCore names them as raw/pack/ca/deepcolour.py gives (asked), from the Core Animation
preferences the stager writes. <out> starts with "# cif10=<answer>" and holds each asked state the manifest does not
name as an R line, which gpu.ca.asked_forms makes on the raw facade.
"""

import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.raw.pack.ca.caspec').origin).parent))
import caspec
import deepcolour


def asked_lines(tables: caspec.Tables, cif10: bool | None) -> list[str]:
    """Return the R lines of the states the device asks for where the manifest names another."""
    named = {m[0] for m in tables.t['manifest']}
    lines = []
    for name, *_ in tables.t['manifest']:
        if name[0] != 'P' or (not tables.v27 and caspec.decode(tables, name).flag('g')):
            continue
        lines += [
            caspec.state_line(tables, form) for form in deepcolour.asked(tables, name, cif10) if form not in named
        ]
    return lines


def main() -> None:
    tables_path, stager, out = sys.argv[1:4]
    cif10 = deepcolour.cif10(Path(stager))
    lines = asked_lines(caspec.Tables(tables_path), cif10)
    answer = 'mobilegestalt' if cif10 is None else str(cif10).lower()
    Path(out).write_text(f'# cif10={answer}\n' + ''.join(line + '\n' for line in lines))


if __name__ == '__main__':
    main()
