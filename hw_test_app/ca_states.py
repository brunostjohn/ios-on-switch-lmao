"""The corpus's Core Animation states this IPSW makes.

ca_states.py <corpus.log> <QuartzCore default.metallib> <qc-<build>.json> <out>. The corpus holds both iOS versions'
states and does not say which made a line, so a state is this IPSW's when its QuartzCore declares both functions and
every constant the line sets on them, and the constants' values are those its QuartzCore makes of the state's name
(raw/pack/ca/caspec.py with this IPSW's tables). iOS 17's path_stroke_* states fail the first on iOS 27, whose
QuartzCore has no such functions; most of iOS 17's other lines fail the second there (a memoryless index past the
pass's attachments, another build's blend function codes), drawing states iOS 27 never makes.
"""

import importlib.util
import sys
from pathlib import Path

from tools.gpu.shaders import metallib

# caspec imports its iOS 27 half (caspec27) by its bare name, so ca/ goes on the path and caspec is imported as caspec
sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.raw.pack.ca.caspec').origin).parent))
import caspec

PREFIX = 'R ok=1 label=com.apple.coreanimation.'


def stage(segment: str, tag: str) -> tuple[str, set[str]] | None:
    """Return a ` | v ...` or ` | f ...` segment's function and the constants it sets."""
    tokens = segment.split()
    if len(tokens) < 2 or tokens[0] != tag:
        return None
    constants = {t.split(':', 1)[0] for t in tokens[2:] if not t.startswith('lib=') and ':' in t}
    return tokens[1], constants


def made_here(line: str, declared: dict[str, set[str]]) -> bool:
    """Whether the QuartzCore whose functions declare `declared` (function -> its constants) makes this state."""
    parts = line.split(' | ')
    if len(parts) < 3:
        return False
    for segment, tag in ((parts[1], 'v'), (parts[2], 'f')):
        found = stage(segment, tag)
        if found is None or found[0] not in declared or not found[1] <= declared[found[0]]:
            return False
    return True


def made_of_name(line: str, tables: caspec.Tables) -> bool:
    """Whether the line's constants are those this QuartzCore makes of its name; a name it does not decode is kept."""

    def constants(fields: str) -> list[str]:
        return sorted(t for t in fields.split() if t.startswith('fc_'))

    try:
        made = caspec.state_fields(tables, caspec.corpus_fields(line)[0])
    except (KeyError, IndexError, ValueError):
        return True
    return constants(made) == constants(line)


def states(corpus: str, library: bytes, tables: caspec.Tables) -> list[str]:
    """Return the corpus's Core Animation lines that `library` (a QuartzCore default.metallib) with `tables` makes."""
    functions = metallib.functions(library)
    if functions is None:
        raise ValueError('not a Metal library')
    declared = {f.name: {c.name for c in f.constants} | {f'#{c.index}' for c in f.constants} for f in functions}
    return [
        line
        for line in corpus.splitlines()
        if line.startswith(PREFIX) and made_here(line, declared) and made_of_name(line, tables)
    ]


def main() -> None:
    corpus, library, tables, out = sys.argv[1:]
    kept = states(Path(corpus).read_text(), Path(library).read_bytes(), caspec.Tables(tables))
    if not kept:
        sys.exit(f"{library} makes none of the corpus's Core Animation states")
    Path(out).write_text(''.join(line + '\n' for line in kept))


if __name__ == '__main__':
    main()
