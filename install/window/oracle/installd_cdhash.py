"""installd's CDHash as overlay.py trust hashes it: what the installer's caller gives switch_window.

installd_cdhash.py MACHO OUT
"""

import sys
from pathlib import Path

from overlay import cdhash

if __name__ == '__main__':
    Path(sys.argv[2]).write_bytes(cdhash(Path(sys.argv[1]).read_bytes()))
