"""A pixel program that writes each column its own colour: the readback experiments' source colours.

The colours are the shader data (constant buffer 1), an RGBA32F each; the program reads its pixel's x through IPA,
converts it, and loads that column's colour with one 128-bit LDC. Each instruction waits on the one before through
barrier 0, so the program is right whatever the latencies are.
"""

import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, operands, sph
from tools.benchloop.sm53.program import DATA_BANK, inputs
from tools.gpu.maxwell import execute

Colour = tuple[float, float, float, float]
MAX_COLUMNS = protocol.MAX_DATA // 16
POSITION_X = 0x70


def ldc128(d: int, offset_reg: int, bank: int) -> encode.Instr:
    """LDC.128 as sm50.rs encodes it: four words of c[bank][offset_reg] into d..d+3."""
    word = 0xEF90 << 48 | d | offset_reg << 8 | encode.PT << 16 | bank << 36 | 6 << 48
    return encode.Instr(word)


def code() -> bytes:
    through = {'wr_bar': 0, 'wait': 1, 'delay': 15}
    body = [
        encode.ipa(4, POSITION_X).sched(wr_bar=0),
        encode.f2i(4, 4, 'u32 rz').sched(**through),
        encode.shl(4, 4, encode.Imm(4)).sched(delay=15, wait=1),
        ldc128(0, 4, DATA_BANK).sched(wr_bar=0),
    ]
    return encode.bundle(encode.finish(body))


def program(colours: list[Colour]) -> dict:
    if not 0 < len(colours) <= MAX_COLUMNS:
        raise ValueError(f'1..{MAX_COLUMNS} columns')
    c = code()
    return {
        'sph': protocol.b64(sph.pixel(operands.attributes(c))),
        'code': protocol.b64(c),
        'data': protocol.b64(b''.join(struct.pack('<4f', *colour) for colour in colours)),
        'gprs': max(8, operands.highest_register(c) + 1),
    }


def run(p: dict, x: int) -> Colour:
    """Give the colour the host interpreter says column x gets."""
    regs, _ = execute.run(protocol.unb64(p['code']), inputs(protocol.unb64(p['data']), (x, 0)), 4)
    return struct.unpack('<4f', struct.pack('<4I', *regs))
