"""The build's own `trust_module` on a set of entry lists: the oracle switch_trustcache is tested against.

Each case is the entry count (4 bytes, big-endian), each entry as CDHash, flags and category (22 bytes), a byte saying
whether a UUID follows and the 16-byte UUID, then a byte that is 0 when `trust_module` returned the module that
follows (length-prefixed like the count) and 1 when it refused.
"""

import random
import struct
import sys
from pathlib import Path

from tools.trustcache.trustcache import trust_module

type Case = tuple[list[tuple[bytes, int, int]], bytes | None]


def _cases() -> list[Case]:
    rng = random.Random(20261126)

    def cdhash() -> bytes:
        return rng.randbytes(20)

    many = [(cdhash(), rng.choice((0, 1, 2)), rng.randrange(256)) for _ in range(300)]
    repeated = many[:50] + many[10:30] + many[:5]
    rng.shuffle(repeated)
    shared = cdhash()
    return [
        ([], None),
        ([(cdhash(), 0, 2)], None),
        ([(cdhash(), 0, 0)], bytes(range(16))),
        (many, None),
        (repeated, None),
        (repeated, rng.randbytes(16)),
        ([(shared, 0, 2), (shared, 0, 3)], None),
        ([(shared, 0, 2), (shared, 1, 2)], None),
        ([(bytes(20), 255, 255), (b'\xff' * 20, 0, 0)], None),
    ]


def main() -> None:
    """Write every case to the file named by the first argument."""
    out = bytearray()
    for entries, uuid in _cases():
        out += struct.pack('>I', len(entries))
        for cdhash, flags, category in entries:
            out += cdhash + bytes([flags, category])
        out += b'\x01' + uuid if uuid else b'\x00' + bytes(16)
        try:
            module = trust_module(entries, uuid)
        except ValueError:
            out += b'\x01'
        else:
            out += b'\x00' + struct.pack('>I', len(module)) + module
    Path(sys.argv[1]).write_bytes(out)


if __name__ == '__main__':
    main()
