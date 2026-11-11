"""Target texels in the formats the bench renders: RGBA floats to bytes as a model of the store, and bytes back."""

import math
import struct
from collections.abc import Callable
from fractions import Fraction

from tools.benchloop import protocol

Colour = tuple[float, float, float, float]
FLOATS = {'rgba16float': (4, '<4e'), 'rgba32float': (4, '<4f'), 'r32float': (1, '<f'), 'rg32float': (2, '<2f')}
UNORM_BITS = {'bgra8unorm': (8, 8, 8, 8), 'rgba8unorm': (8, 8, 8, 8), 'rgb10a2unorm': (10, 10, 10, 2)}


def round_half_even(x: Fraction) -> int:
    floor = math.floor(x)
    rest = x - floor
    return floor + (rest > Fraction(1, 2) or (rest == Fraction(1, 2) and floor % 2 == 1))


# float -> unorm code models the ROP experiments tell apart: each takes the f32's exact value and the code's maximum.
# Nearest needs no tie rule: with 2^n - 1 codes the only exact tie is 0.5, where even and away agree.
ROUNDING: dict[str, Callable[[float, int], int]] = {
    'nearest': lambda v, top: round_half_even(Fraction(v) * top),
    'truncate': lambda v, top: math.floor(Fraction(v) * top),
    'f32_mul_add_half': lambda v, top: math.floor(f32(f32(v * top) + 0.5)),
}


def f32(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', x))[0] if math.isfinite(x) and abs(x) < 3.4e38 else x


def unorm(v: float, bits: int, rounding: str = 'nearest') -> int:
    top = (1 << bits) - 1
    if math.isnan(v) or v <= 0:
        return 0
    if v >= 1:
        return top
    return min(top, ROUNDING[rounding](f32(v), top))


def encode(colour: Colour, fmt: str, rounding: str = 'nearest') -> bytes:
    if fmt in UNORM_BITS:
        bits = UNORM_BITS[fmt]
        codes = [unorm(c, n, rounding) for c, n in zip(colour, bits, strict=True)]
        if fmt == 'bgra8unorm':
            return bytes([codes[2], codes[1], codes[0], codes[3]])
        if fmt == 'rgba8unorm':
            return bytes(codes)
        return struct.pack('<I', codes[0] | codes[1] << 10 | codes[2] << 20 | codes[3] << 30)
    lanes, layout = FLOATS[fmt]
    return struct.pack(layout, *colour[:lanes])


def codes(texel: bytes, fmt: str) -> tuple[int, ...]:
    """Give a unorm texel's codes in RGBA order."""
    if fmt == 'bgra8unorm':
        return texel[2], texel[1], texel[0], texel[3]
    if fmt == 'rgba8unorm':
        return tuple(texel)
    if fmt == 'rgb10a2unorm':
        w = struct.unpack('<I', texel)[0]
        return w & 0x3FF, w >> 10 & 0x3FF, w >> 20 & 0x3FF, w >> 30
    raise ValueError(f'{fmt} is not a unorm format')


def decode(texel: bytes, fmt: str) -> Colour:
    if fmt in UNORM_BITS:
        return tuple(c / ((1 << n) - 1) for c, n in zip(codes(texel, fmt), UNORM_BITS[fmt], strict=True))
    if fmt == 'rgba16float':
        return struct.unpack('<4e', texel)
    if fmt == 'rgba32float':
        return struct.unpack('<4f', texel)
    if fmt == 'r32float':
        return struct.unpack('<f', texel)[0], 0.0, 0.0, 1.0
    if fmt == 'rg32float':
        return (*struct.unpack('<2f', texel), 0.0, 1.0)
    raise ValueError(fmt)


def split(data: bytes, fmt: str) -> list[bytes]:
    size = protocol.FORMATS[fmt][1]
    return [data[i : i + size] for i in range(0, len(data), size)]
