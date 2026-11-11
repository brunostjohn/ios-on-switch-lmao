"""rop.py's store rule against the device bench's readings and an exact reference."""

import math
import random
import struct
import unittest
from fractions import Fraction

from tools.gpu.maxwell import rop


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reference(bits: int, n: int) -> int:
    v = struct.unpack('<f', struct.pack('<I', bits))[0]
    if math.isnan(v) or v <= 0:
        return 0
    if v >= 1:
        return (1 << n) - 1
    u = int(Fraction(v) * (1 << (n + 4)))
    return (u * ((1 << n) - 1) + (1 << (n + 3)) - 1) >> (n + 4)


class RopTest(unittest.TestCase):
    def test_bench_readings(self):
        # diag4 (20261111-155823), bgra8unorm
        for value, code in ((0.25, 0x40), (0.5, 0x7F), (1.0, 0xFF), (0.75, 0xBF)):
            self.assertEqual(rop.store_code(f32(value), 8), code)
        for k in range(6):
            self.assertEqual(rop.store_code(f32((k + 0.5) / 255), 8), k)
        self.assertEqual(rop.store_code(0x7FC00000, 8), 0)
        self.assertEqual(rop.store_code(0x7F800000, 8), 255)
        self.assertEqual(rop.store_code(f32(-0.0), 10), 0)
        self.assertEqual([rop.store_code(f32(c / 3), rop.ALPHA2) for c in range(4)], [0, 1, 2, 3])

    def test_matches_the_exact_rule(self):
        rng = random.Random(4)
        values = [rng.getrandbits(32) for _ in range(20000)] + [f32(rng.random()) for _ in range(20000)]
        values += list(range(0, 0x800000, 4099))
        for n in (8, 10, 16):
            for bits in values:
                self.assertEqual(rop.store_code(bits, n), reference(bits, n), (hex(bits), n))

    def test_output_widths(self):
        key = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1 c1=115:0:0:1 c2=90:15:0:1  | v x  | f y'
        self.assertEqual(rop.widths(key, 0xF | 0x3 << 4 | 0x9 << 8), [8, 8, 8, 8, None, None, 10, rop.ALPHA2])


if __name__ == '__main__':
    unittest.main()
