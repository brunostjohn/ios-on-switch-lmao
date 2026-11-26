"""The build's own installd patch (arm64 slice plus LC_LOAD_DYLIB) before it is re-signed: switch_sign's test input."""

import sys
from pathlib import Path

from tools.install_policy.installd import inject


def main() -> None:
    """Write inject(<installd>) to <out>."""
    source, out = (Path(arg) for arg in sys.argv[1:3])
    out.write_bytes(inject(source.read_bytes()))


if __name__ == '__main__':
    main()
