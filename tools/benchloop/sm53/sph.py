"""The 80-byte pixel shader program header (SPH version 3, type 2) as NAK's sph.rs writes it for a fragment shader.

Bit positions are cla097sph.h's NVA097_SPHV3T2_* fields.
"""

SPH_BYTES = 80
SPH_TYPE_PS = 2
SPH_VERSION = 3
SHADER_TYPE_PIXEL = 5
SASS_VERSION = 1
IMAP_SYSTEM_VALUES_AB = 160
IMAP_GENERIC = 192
OMAP_TARGET = 576
OMAP_SAMPLE_MASK = 608
OMAP_DEPTH = 609
FIRST_GENERIC = 0x80
LAST_GENERIC = 0x27C
# sph.rs PixelImap by IPA frequency, as NAK's from_nir pairs them: pass is screen-linear, pass*w perspective
IMAP_OF_FREQ = {0: 3, 1: 2, 2: 1}


def _put(header: int, at: int, n: int, value: int) -> int:
    if not 0 <= value < 1 << n:
        raise ValueError(f'{value:#x} does not fit SPH bits {at}..{at + n - 1}')
    return header | value << at


def pixel(attributes: dict[int, int] | None = None, targets: int = 0xF, kills: bool = False) -> bytes:
    """Give the header of a fragment program that reads `attributes` (byte address: IPA frequency).

    `targets` is the output map, four bits (RGBA) per render target from target 0 up. MRT is enabled as NAK always
    enables it: each target takes its own registers rather than a broadcast of the first.
    """
    h = _put(0, 0, 5, SPH_TYPE_PS)
    h = _put(h, 5, 5, SPH_VERSION)
    h = _put(h, 10, 4, SHADER_TYPE_PIXEL)
    h = _put(h, 14, 1, 1)
    h = _put(h, 15, 1, int(kills))
    h = _put(h, 17, 4, SASS_VERSION)
    for addr, freq in (attributes or {}).items():
        if addr < FIRST_GENERIC:
            h |= 1 << (IMAP_SYSTEM_VALUES_AB + addr // 4)
        elif addr <= LAST_GENERIC:
            h = _put(h, IMAP_GENERIC + (addr - FIRST_GENERIC) // 4 * 2, 2, IMAP_OF_FREQ[freq])
        else:
            raise ValueError(f'attribute {addr:#x} has no input map here')
    h = _put(h, OMAP_TARGET, 32, targets)
    return h.to_bytes(SPH_BYTES, 'little')
