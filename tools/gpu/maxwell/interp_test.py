"""interp.py's arithmetic against the host's IEEE, and the gate's eye for a one-bit codegen change."""

import math
import random
import struct
import unittest
from pathlib import Path

from tools.gpu.maxwell import execute, floats, gate, isa, texture
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


class ImmediateTest(unittest.TestCase):
    def test_f2f_immediate_is_a_floats_top_bits(self):
        # F2F.F16.F32 r2, 1.0 and -2.0 (nv50_ir emitF2F: imm20 is the f32's top 20 bits, its sign at bit 56)
        for value, half in ((0x3F800000, 0x3C00), (0xC0000000, 0xC000)):
            w = 0x38A8 << 48 | (value >> 31) << 56 | (value >> 12 & 0x7FFFF) << 20 | 2 << 10 | 1 << 8 | 2
            lane = Lane()
            execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
            self.assertEqual(lane.r(2), half)

    def test_fadd_saturates_where_32i_cannot(self):
        # FADD.SAT r2, r1, 2.0 (imm20) clamps to 1.0; FADD32I has no SAT bit, so NAK must not choose it
        w = 0x3858 << 48 | 1 << 50 | (0x40000000 >> 12 & 0x7FFFF) << 20 | 1 << 8 | 2
        lane = Lane()
        lane.w(1, 0x3F000000)
        execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
        self.assertEqual(lane.r(2), 0x3F800000)


class ThreeSourceTest(unittest.TestCase):
    def run_one(self, w: int, regs: dict) -> int:
        lane = Lane()
        for r, v in regs.items():
            lane.w(r, v)
        execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
        return lane.r(4)

    def test_lop3_iadd3_iscadd(self):
        regs = {1: 0x12345678, 2: 0x0F0F0F0F, 3: 0xFF00FF00}
        # LOP3.LUT r4, r1, r2, r3, 0x96 (a ^ b ^ c); with a 20-bit immediate b, 0xE8 (majority)
        lop3 = 0x5BE7 << 48 | 3 << 39 | 0x96 << 28 | 2 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(lop3, regs), 0x12345678 ^ 0x0F0F0F0F ^ 0xFF00FF00)
        lop3i = 0x3C00 << 48 | 0xE8 << 48 | 3 << 39 | 0x7FFF << 20 | 1 << 8 | 4
        a, b, c = 0x12345678, 0x7FFF, 0xFF00FF00
        self.assertEqual(self.run_one(lop3i, regs), (a & b) | (a & c) | (b & c))
        # IADD3 r4, r1, -r2, r3
        iadd3 = 0x5CC0 << 48 | 1 << 50 | 3 << 39 | 2 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(iadd3, regs), (0x12345678 - 0x0F0F0F0F + 0xFF00FF00) & 0xFFFFFFFF)
        # ISCADD r4, r1, 0x40, 3: (r1 << 3) + 0x40
        iscadd = 0x3818 << 48 | 3 << 39 | 0x40 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(iscadd, regs), (0x12345678 << 3) + 0x40 & 0xFFFFFFFF)


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


if __name__ == '__main__':
    unittest.main()
