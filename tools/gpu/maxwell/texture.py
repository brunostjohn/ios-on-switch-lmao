"""Maxwell's texture fetches on the host: TEX (bindless and bound), TEXS, TLD and TLDS, as interp.py's lanes run them.

A fetch is a pure function of a canonical key, so two programs that sample the same texel the same way get the same
result whatever form each uses: the handle word (a bindless one's register, a bound one's constant buffer word at its
index in the buffer SET_BINDLESS_TEXTURE names), the coordinates (an array's index first), the level mode (0 implicit,
1 zero, 2 bias, 3 level) and its value, the depth reference, the offsets and the dimension (TEX's encoding). A texel
load (TLD, TLDS) is keyed apart from a sample: its handle, integer coordinates, level (none for level zero), sample
index, offsets and dimension. Field layouts are NAK's sm50.rs; the scalar forms' targets, masks and F16 packing those
of the Switch emulators' decoders (texture_fetch_swizzled.cpp, texture_load_swizzled.cpp). A scalar F16 result is the
f32 one rounded to the nearest half, ties to even: the assumption a pack may hold only once the device's probe has
shown it (raw_chip_facts.h).
"""

from tools.gpu.maxwell.floats import F16, F32, RN, convert
from tools.gpu.maxwell.interp import RZ_REG, Inputs, Lane, UnsupportedError
from tools.gpu.maxwell.isa import field as bits

DIM_COORDS = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 6: 3, 7: 4}
LEVEL_OPERAND = (False, False, True, True)  # by level mode: implicit, zero, bias, level
DIM_1D, DIM_2D, DIM_A2D, DIM_3D, DIM_CUBE = 0, 2, 3, 4, 6
# TEXS's targets (bits 53-56): dimension, level mode, depth reference, then where each operand is (a: register A's
# first, a1: its second, b, b1 the same of register B)
TEXS_TARGETS = {
    0: (DIM_1D, 1, False, ('a',), None, None),
    1: (DIM_2D, 0, False, ('a', 'b'), None, None),
    2: (DIM_2D, 1, False, ('a', 'b'), None, None),
    3: (DIM_2D, 3, False, ('a', 'a1'), 'b', None),
    4: (DIM_2D, 0, True, ('a', 'a1'), None, 'b'),
    5: (DIM_2D, 3, True, ('a', 'a1'), 'b', 'b1'),
    6: (DIM_2D, 1, True, ('a', 'a1'), None, 'b'),
    7: (DIM_A2D, 0, False, ('a', 'a1', 'b'), None, None),
    8: (DIM_A2D, 1, False, ('a', 'a1', 'b'), None, None),
    9: (DIM_A2D, 1, True, ('a', 'a1', 'b'), None, 'b1'),
    10: (DIM_3D, 0, False, ('a', 'a1', 'b'), None, None),
    11: (DIM_3D, 1, False, ('a', 'a1', 'b'), None, None),
    12: (DIM_CUBE, 0, False, ('a', 'a1', 'b'), None, None),
    13: (DIM_CUBE, 3, False, ('a', 'a1', 'b'), 'b1', None),
}
# TLDS's targets (bits 53-56): dimension, then where its coordinates, level, sample index and offsets are (as TEXS's)
TLDS_TARGETS = {
    0: (DIM_1D, ('a',), None, None, None),
    1: (DIM_1D, ('a',), 'b', None, None),
    2: (DIM_2D, ('a', 'b'), None, None, None),
    4: (DIM_2D, ('a', 'a1'), None, None, 'b'),
    5: (DIM_2D, ('a', 'a1'), 'b', None, None),
    6: (DIM_2D, ('a', 'a1'), None, 'b', None),
    7: (DIM_3D, ('a', 'a1', 'b'), None, None, None),
    8: (DIM_A2D, ('a', 'b', 'b1'), None, None, None),
    12: (DIM_2D, ('a', 'a1'), 'b', None, 'b1'),
}
R, G, B, A = 1, 2, 4, 8
RG_LUT = (R, G, B, A, R | G, R | A, G | A, B | A)
RGBA_LUT = (R | G | B, R | G | A, R | B | A, G | B | A, R | G | B | A)


def reg(lane: Lane, r: int) -> int:
    return 0 if r == RZ_REG else lane.r(r)


def fetch(inputs: Inputs, handle: int, coords: tuple, mode: int, level, dref, offset, dim: int) -> list[int]:  # noqa: ANN001, PLR0913, PLR0917  the canonical key's parts
    return inputs.texture(('tex', handle, coords, mode, level, dref, offset, dim))


def load(inputs: Inputs, handle: int, coords: tuple, level, sample, offset, dim: int) -> list[int]:  # noqa: ANN001, PLR0913, PLR0917  the canonical key's parts
    return inputs.texture(('tld', handle, coords, level, sample, offset, dim))


def write_compact(lane: Lane, d: int, mask: int, result: list[int]) -> None:
    out = 0
    for c in range(4):
        if mask >> c & 1:
            lane.w(d + out, result[c])
            out += 1


def tex(name: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """TEX: bindless (TEX_b, the handle the first word of register B's operands) or bound (the handle at its index)."""
    if name not in ('TEX', 'TEX_b'):
        raise UnsupportedError(name)
    bindless = name == 'TEX_b'
    dim, mask = bits(w, 28, 3), bits(w, 31, 4)
    mode = bits(w, 37, 2) if bindless else bits(w, 55, 2)
    aoffi = bits(w, 36, 1) if bindless else bits(w, 54, 1)
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)
    coords = tuple(reg(lane, ra + k if ra != RZ_REG else RZ_REG) for k in range(DIM_COORDS[dim]))
    operands = [reg(lane, rb + k if rb != RZ_REG else RZ_REG) for k in range(5)]
    handle = operands.pop(0) if bindless else inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    level = operands.pop(0) if LEVEL_OPERAND[mode] else None
    offset = operands.pop(0) if aoffi else None
    dref = operands.pop(0) if bits(w, 50, 1) else None
    write_compact(lane, d, mask, fetch(inputs, handle, coords, mode, level, dref, offset, dim))


def tld(name: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """TLD: bindless (TLD_b, the handle register B's first operand) or bound, then level, offsets and sample."""
    if bits(w, 54, 1):
        raise UnsupportedError('TLD.CL')
    dim, mask = bits(w, 28, 3), bits(w, 31, 4)
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)
    coords = tuple(reg(lane, ra + k if ra != RZ_REG else RZ_REG) for k in range(DIM_COORDS[dim]))
    operands = [reg(lane, rb + k if rb != RZ_REG else RZ_REG) for k in range(4)]
    handle = operands.pop(0) if name == 'TLD_b' else inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    level = operands.pop(0) if bits(w, 55, 1) else None
    offset = operands.pop(0) if bits(w, 35, 1) else None
    sample = operands.pop(0) if bits(w, 50, 1) else None
    write_compact(lane, d, mask, load(inputs, handle, coords, level, sample, offset, dim))


def scalar_operand(w: int, lane: Lane, place: str | None):  # noqa: ANN201  a word, or None where the target has none
    """Give a scalar form's operand: register A's (a, a1) or B's (b, b1) first or second word."""
    if place is None:
        return None
    base = bits(w, 8, 8) if place[0] == 'a' else bits(w, 20, 8)
    return reg(lane, base + len(place) - 1 if base != RZ_REG else RZ_REG)


def write_scalar(w: int, lane: Lane, result: list[int]) -> None:
    """Write a scalar form's components as its mask picks them, in A's register pair then B's: f32 words or halves."""
    da, db = bits(w, 0, 8), bits(w, 28, 8)
    swizzle = bits(w, 50, 3)
    lut = RG_LUT if db == RZ_REG else RGBA_LUT
    if swizzle >= len(lut):
        raise UnsupportedError(f'scalar texture mask {swizzle}')
    picked = [result[c] for c in range(4) if lut[swizzle] >> c & 1]
    targets = [da, da + 1, db, db + 1]
    if bits(w, 59, 1):
        for k, v in enumerate(picked):
            lane.w(targets[k], v)
        return
    halves = [convert(v, F32, F16, RN) for v in picked] + [0] * (4 - len(picked))
    lane.w(da, halves[0] | halves[1] << 16)
    if len(picked) > 2:
        lane.w(db, halves[2] | halves[3] << 16)


def texs(w: int, lane: Lane, inputs: Inputs) -> None:
    """TEXS: its operands in registers A and B as its target places them, its components in A's pair then B's."""
    target = bits(w, 53, 4)
    if target not in TEXS_TARGETS:
        raise UnsupportedError(f'TEXS target {target}')
    dim, mode, _, where_coords, where_level, where_dref = TEXS_TARGETS[target]
    handle = inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    coords = tuple(scalar_operand(w, lane, p) for p in where_coords)
    level, dref = scalar_operand(w, lane, where_level), scalar_operand(w, lane, where_dref)
    write_scalar(w, lane, fetch(inputs, handle, coords, mode, level, dref, None, dim))


def tlds(w: int, lane: Lane, inputs: Inputs) -> None:
    """TLDS: a bound texel load, its operands in registers A and B as its target places them."""
    target = bits(w, 53, 4)
    if target not in TLDS_TARGETS:
        raise UnsupportedError(f'TLDS target {target}')
    dim, where_coords, where_level, where_sample, where_offset = TLDS_TARGETS[target]
    handle = inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    coords = tuple(scalar_operand(w, lane, p) for p in where_coords)
    level, sample, offset = (scalar_operand(w, lane, p) for p in (where_level, where_sample, where_offset))
    write_scalar(w, lane, load(inputs, handle, coords, level, sample, offset, dim))
