"""GM20B's store of a fragment output into a unorm colour channel, as the device bench measured it.

rop1 (bench loop 20261111-155957, 254,892 of 254,892 readings of BGRA8, RGBA8 and RGB10A2, blended with ONE/ZERO,
ONE/ONE or not at all) and blendff (20261111-161623, the same rule for the source of every blend): a NaN or a value
from 0 down stores 0, a value from 1 up the maximum, and any other is truncated to n + 4 fractional bits and its
product with the maximum 2^n - 1 rounded to nearest, ties down. RGB10A2's two-bit alpha is the top two bits of the
10-bit code. 16-bit channels follow the rule in a cross-check against an independent probe, not a direct sweep.
"""

ALPHA2 = 'a2'

# Metal pixel formats whose channels store as unorm codes, each channel's width (the shader's RGBA order)
UNORM = {
    10: (8,),
    30: (8, 8),
    70: (8, 8, 8, 8),
    80: (8, 8, 8, 8),
    20: (16,),
    60: (16, 16),
    110: (16, 16, 16, 16),
    90: (10, 10, 10, ALPHA2),
    94: (10, 10, 10, ALPHA2),
}


def store_code(bits: int, width: int | str) -> int:
    """Give the code GM20B stores for the f32 `bits` in a unorm channel of `width` bits (or RGB10A2's alpha)."""
    if width == ALPHA2:
        return store_code(bits, 10) >> 8
    exponent = bits >> 23 & 0xFF
    if bits >> 31 or bits & 0x7FFFFFFF == 0 or (exponent == 0xFF and bits & 0x7FFFFF):
        return 0
    top = (1 << width) - 1
    if exponent >= 127:
        return top
    mantissa = (bits & 0x7FFFFF) | (0x800000 if exponent else 0)
    shift = max(exponent, 1) - 127 - 23 + width + 4
    u = mantissa << shift if shift >= 0 else mantissa >> -shift
    return (u * top + (1 << (width + 3)) - 1) >> (width + 4)


def widths(key: str, omap: int) -> list[int | str | None]:
    """Give each output register's unorm channel width (None: not a unorm channel) for a state key's attachments.

    The output registers hold the written components of each target in turn (the program header's output map).
    """
    formats = {}
    for field in key.split():
        if field[0] == 'c' and field[1].isdigit() and '=' in field:
            target, rest = field[1:].split('=', 1)
            formats[int(target)] = int(rest.split(':', 1)[0])
    out = []
    for target in range(8):
        for component in range(4):
            if omap >> (4 * target + component) & 1:
                channels = UNORM.get(formats.get(target, -1), ())
                out.append(channels[component] if component < len(channels) else None)
    return out
