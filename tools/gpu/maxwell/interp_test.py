"""interp.py's arithmetic against the host's IEEE, and the gate's eye for a one-bit codegen change."""

import math
import random
import struct
import unittest
from pathlib import Path

from tools.gpu.maxwell import floats, gate, isa, texture
from tools.gpu.maxwell.execute import run
from tools.gpu.maxwell.interp import Lane

FIXTURE = Path(__file__).with_name('testdata') / 'image_draw_fragment.hex'
F2F_NEG = 1 << 45
ONE_TARGET_RGBA = (
    bytes(gate.OMAP_TARGET_BYTE) + struct.pack('<I', 0xF) + bytes(isa.HEADER_BYTES - gate.OMAP_TARGET_BYTE - 4)
)


def f32(x: float) -> int:
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return 0xFF800000 if x < 0 else 0x7F800000


def f16(x: float) -> int:
    try:
        return struct.unpack('<H', struct.pack('<e', x))[0]
    except OverflowError:
        return 0xFC00 if x < 0 else 0x7C00


def host(bits: int, fmt: str) -> float:
    return struct.unpack('<' + fmt, struct.pack('<' + ('I' if fmt == 'f' else 'H'), bits))[0]


class FloatsTest(unittest.TestCase):
    def test_f32_add_mul_round_like_the_host(self):
        rng = random.Random(1)
        for _ in range(5000):
            a, b = rng.getrandbits(32), rng.getrandbits(32)
            if floats.is_nan(a, floats.F32) or floats.is_nan(b, floats.F32):
                continue
            x, y = host(a, 'f'), host(b, 'f')
            for op, want in (('add', x + y), ('mul', x * y)):
                expect = floats.F32.nan if math.isnan(want) else f32(want)
                self.assertEqual(floats.arith(op, [a, b], floats.F32), expect, (op, hex(a), hex(b)))

    def test_f16_add_mul_and_narrowing(self):
        rng = random.Random(2)
        for _ in range(5000):
            a, b = rng.getrandbits(16), rng.getrandbits(16)
            if floats.is_nan(a, floats.F16) or floats.is_nan(b, floats.F16):
                continue
            x, y = host(a, 'e'), host(b, 'e')
            for op, want in (('add', x + y), ('mul', x * y)):
                expect = floats.F16.nan if math.isnan(want) else f16(want)
                self.assertEqual(floats.arith(op, [a, b], floats.F16), expect, (op, hex(a), hex(b)))
            c = rng.getrandbits(32)
            if not floats.is_nan(c, floats.F32):
                self.assertEqual(floats.convert(c, floats.F32, floats.F16), f16(host(c, 'f')), hex(c))

    def test_half_fma_rounds_once(self):
        a, b, c = 0x3C01, 0x3C01, 0x8000 | 0x3C02  # (1+u)^2 - (1+2u) = u^2: lost by two roundings
        self.assertNotEqual(floats.arith('fma', [a, b, c], floats.F16), 0)


class GateTest(unittest.TestCase):
    def setUp(self):
        self.code = isa.from_hex_dump(FIXTURE.read_text())

    def test_runs_to_exit(self):
        regs, killed = run(self.code, gate.Seeded(1, 0).inputs(), 4)
        self.assertEqual(len(regs), 4)
        self.assertFalse(killed)

    def test_identical_to_itself(self):
        header = ONE_TARGET_RGBA
        self.assertEqual(gate.compare((header + self.code, -1), (header + self.code, -1), 30, 1), 'identical')

    def test_sees_one_flipped_bit(self):
        words = list(struct.unpack(f'<{len(self.code) // 8}Q', self.code))
        at = max(i for i, w in enumerate(words) if i % 4 and isa.name(w) == 'F2F_reg')
        words[at] ^= F2F_NEG
        mutated = struct.pack(f'<{len(words)}Q', *words)
        header = ONE_TARGET_RGBA
        self.assertTrue(gate.compare((header + self.code, -1), (header + mutated, -1), 50, 1).startswith('different'))


class TextureTest(unittest.TestCase):
    # TEX_b r4, r0 (x, y), r2 (the handle) .2d rgba; TEXS r8:r10, r0 (x), r1 (y) .2d rgba at bound index 16
    TEX_B = 0xDE80_0000_0000_0000 | 0xF << 31 | 2 << 28 | 2 << 20 | 0 << 8 | 4
    TEXS = 0xD800_0000_0000_0000 | 1 << 53 | 4 << 50 | 16 << 36 | 10 << 28 | 1 << 20 | 0 << 8 | 8

    def lanes(self) -> tuple:
        inputs = gate.Seeded(3, 0).inputs(0, bound_cbuf=1)
        lane = Lane()
        lane.w(0, 0x3E800000)
        lane.w(1, 0x3F400000)
        lane.w(2, inputs.cbuf(1, 64))
        return inputs, lane

    def test_bound_scalar_and_bindless_forms_fetch_the_same_texel(self):
        inputs, lane = self.lanes()
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 4)
        texture.texs(self.TEXS, lane, inputs)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (8, 9, 10, 11)])

    def test_texs_f16_packs_round_to_nearest_even_halves(self):
        inputs, lane = self.lanes()
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 4)
        texture.texs(self.TEXS & ~(1 << 59), lane, inputs)
        want = [floats.convert(lane.r(r), floats.F32, floats.F16) for r in (4, 5, 6, 7)]
        self.assertEqual((lane.r(8), lane.r(10)), (want[0] | want[1] << 16, want[2] | want[3] << 16))

    # TLD_b r4, r0 (x, y), r2 (the handle) .2d rgba, level zero; TLDS r8:r10, r0 (x), r1 (y) .2d level zero at bound
    # index 16
    TLD_B = 0xDD38 << 48 | 0xF << 31 | 2 << 28 | 2 << 20 | 0 << 8 | 4
    TLDS = 0xD << 60 | 1 << 59 | 1 << 57 | 2 << 53 | 4 << 50 | 16 << 36 | 10 << 28 | 1 << 20 | 0 << 8 | 8

    def test_bound_scalar_and_bindless_loads_read_the_same_texel(self):
        inputs, lane = self.lanes()
        texture.tld('TLD_b', self.TLD_B, lane, inputs, 4)
        texture.tlds(self.TLDS, lane, inputs)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (8, 9, 10, 11)])

    def test_a_load_and_a_sample_of_one_texel_are_different_keys(self):
        inputs, lane = self.lanes()
        texture.tld('TLD_b', self.TLD_B, lane, inputs, 4)
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 12)
        self.assertNotEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (12, 13, 14, 15)])


if __name__ == '__main__':
    unittest.main()
