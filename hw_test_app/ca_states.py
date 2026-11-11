"""ca_states.py <corpus.log> <QuartzCore default.metallib> <out>: the corpus's Core Animation states this IPSW makes.

The corpus holds both iOS versions' states and does not say which made a line, so a state is this IPSW's when its
QuartzCore declares both functions and every constant the line sets on them. iOS 17's path_stroke_* states fail that
on iOS 27, whose QuartzCore has no such functions.
"""

import sys
from pathlib import Path

from tools.gpu.shaders import metallib

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


def states(corpus: str, library: bytes) -> list[str]:
    """Return the corpus's Core Animation lines that `library` (a QuartzCore default.metallib) makes."""
    functions = metallib.functions(library)
    if functions is None:
        raise ValueError('not a Metal library')
    declared = {f.name: {c.name for c in f.constants} | {f'#{c.index}' for c in f.constants} for f in functions}
    return [line for line in corpus.splitlines() if line.startswith(PREFIX) and made_here(line, declared)]


def main() -> None:
    corpus, library, out = sys.argv[1:]
    kept = states(Path(corpus).read_text(), Path(library).read_bytes())
    if not kept:
        sys.exit(f"{library} makes none of the corpus's Core Animation states")
    Path(out).write_text(''.join(line + '\n' for line in kept))


if __name__ == '__main__':
    main()
