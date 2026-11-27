"""Blocks the kernel window can hold, each with the frame `lz4 -12 -B6 --no-frame-crc` makes of it.

    lz4_cases.py LZ4 OUT

Each case is the block and then its frame, each length-prefixed (4 bytes, big-endian).
"""

import random
import struct
import subprocess
import sys
from pathlib import Path

MIB = 1 << 20


def _blocks() -> list[bytes]:
    rng = random.Random(20261126)
    words = [rng.randbytes(8) for _ in range(64)]

    def code(size: int) -> bytes:
        return b''.join(rng.choice(words) + bytes(rng.choice((0, 0, 8))) for _ in range(size // 12 + 1))[:size]

    sparse = bytearray(MIB)
    for at in rng.sample(range(MIB), 40):
        sparse[at] = rng.randrange(1, 256)
    mixed = bytearray(code(MIB))
    mixed[300_000:500_000] = rng.randbytes(200_000)
    return [
        code(MIB),
        rng.randbytes(MIB),
        code(0x1000),
        rng.randbytes(0x1000),
        code(0x10000),
        code(0x11000),
        code(0x32000),
        bytes(sparse),
        bytes(mixed),
        bytes(MIB),
        code(MIB - 0x1000),
    ]


def main() -> None:
    """Write every case to OUT."""
    lz4, out = sys.argv[1], bytearray()
    for block in _blocks():
        frame = subprocess.run(
            [lz4, '-12', '-B6', '--no-frame-crc', '-c', '-q'], input=block, capture_output=True, check=True
        ).stdout
        out += struct.pack('>I', len(block)) + block + struct.pack('>I', len(frame)) + frame
    Path(sys.argv[2]).write_bytes(out)


if __name__ == '__main__':
    main()
