"""Bench programs around a measured body: registers loaded from the shader data, results packed into one RGBA32F texel.

The shader data is bound at c[1]; the prologue moves word i into the i-th initial register. The epilogue writes the
two 32-bit results x and y as 0x3f800000 | (x & 0xffff), 0x3f800000 | (x >> 16) and the same for y: normal floats in
[1, 2), which the ROP stores bit for bit whatever x and y are (denormals, NaNs, integers). Delays outside the body are
15 and the body is followed by a wait on every barrier, so only the body's own scheduling is measured.
"""

import struct
from dataclasses import dataclass

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, operands, sph
from tools.benchloop.sm53.encode import ALL_BARRIERS, RZ, Cbuf, Instr
from tools.gpu.maxwell import execute
from tools.gpu.maxwell.interp import Inputs, UnsupportedError

DATA_BANK = 1
SAFE_DELAY = 15
ONE = 0x3F800000
OUTPUT_REGISTERS = 4
POSITION = (0x70, 0x74)
SEPARATOR = encode.nop().sched(delay=SAFE_DELAY, wait=ALL_BARRIERS)


@dataclass(frozen=True)
class Program:
    code: bytes
    data: bytes
    gprs: int
    sph: bytes | None = None

    def wire(self) -> dict:
        """Give the program as a protocol experiment carries it."""
        out = {'code': protocol.b64(self.code), 'data': protocol.b64(self.data), 'gprs': self.gprs}
        if self.sph is not None:
            out['sph'] = protocol.b64(self.sph)
        return out


def _scheduled(instrs: list[Instr]) -> list[Instr]:
    """Give independent setup instructions issued a cycle apart, the last one SAFE_DELAY before what follows."""
    return [i.sched(delay=SAFE_DELAY if k == len(instrs) - 1 else 1) for k, i in enumerate(instrs)]


def prologue(init: dict[int, int], preds: dict[int, bool] | None = None) -> list[Instr]:
    moves = [encode.mov(r, Cbuf(DATA_BANK, 4 * k)) for k, r in enumerate(sorted(init))]
    setps = [encode.isetp(p, RZ, RZ, 'eq' if v else 'ne') for p, v in sorted((preds or {}).items())]
    return _scheduled(moves + setps)


def data_of(init: dict[int, int]) -> bytes:
    words = [init[r] & encode.MASK32 for r in sorted(init)]
    words += [0] * (-len(words) % 4)
    return struct.pack(f'<{len(words)}I', *words)


def epilogue(results: tuple[int, int]) -> list[Instr]:
    if set(results) & set(range(OUTPUT_REGISTERS)):
        raise ValueError(f'results {results} would be overwritten by the outputs R0..R3')
    x, y = results
    split = [
        encode.lop32i(0, x, 0xFFFF, 'and'),
        encode.shr(1, x, encode.Imm(16)),
        encode.lop32i(2, y, 0xFFFF, 'and'),
        encode.shr(3, y, encode.Imm(16)),
    ]
    mark = [encode.lop32i(r, r, ONE, 'or') for r in range(OUTPUT_REGISTERS)]
    return _scheduled(split) + _scheduled(mark)


def _gprs(code: bytes) -> int:
    return max(OUTPUT_REGISTERS, operands.highest_register(code) + 1)


def fragment(
    body: list[Instr], init: dict[int, int], results: tuple[int, int], preds: dict[int, bool] | None = None
) -> Program:
    """Give a fragment program: load `init` and `preds`, run `body`, write `results` to render target 0."""
    code = encode.bundle(encode.finish([*prologue(init, preds), *body, SEPARATOR, *epilogue(results)]))
    return Program(code, data_of(init), _gprs(code), sph.pixel(operands.attributes(code)))


def compute(body: list[Instr], init: dict[int, int], preds: dict[int, bool] | None = None) -> Program:
    """Give the compute form of a body: the same prologue, no outputs."""
    code = encode.bundle(encode.finish([*prologue(init, preds), *body, SEPARATOR]))
    return Program(code, data_of(init), _gprs(code))


def inputs(data: bytes, position: tuple[int, int] | None = None) -> Inputs:
    """Give the interpreter's environment: `data` as c[1], `position` the pixel whose centre IPA reads at 0x70/0x74."""

    def cbuf(bank: int, offset: int) -> int:
        if bank != DATA_BANK or offset + 4 > len(data):
            return 0
        return struct.unpack_from('<I', data, offset)[0]

    def attribute(addr: int) -> int:
        if position is None or addr not in POSITION:
            return 0
        return encode.f32(position[POSITION.index(addr)] + 0.5)

    def texture(_: tuple) -> list[int]:
        raise UnsupportedError('bench programs sample no textures')

    return Inputs(attribute, cbuf, texture)


def expected_texel(code: bytes, data: bytes, position: tuple[int, int] | None = None) -> list[int]:
    """Give the four words a fragment program writes, as the host interpreter runs it."""
    regs, _ = execute.run(code, inputs(data, position), OUTPUT_REGISTERS)
    return regs


def pack(x: int, y: int) -> list[int]:
    return [ONE | (x & 0xFFFF), ONE | (x >> 16 & 0xFFFF), ONE | (y & 0xFFFF), ONE | (y >> 16 & 0xFFFF)]


def unpack(texel: list[int]) -> tuple[int, int]:
    w0, w1, w2, w3 = (w & 0xFFFF for w in texel)
    return w0 | w1 << 16, w2 | w3 << 16


def texel_hex(texel: list[int]) -> str:
    """Give a texel as the device reports it: the 16 bytes of the RGBA32F texel, little endian, in hex."""
    return struct.pack('<4I', *texel).hex()


def texel_words(text: str) -> list[int]:
    return list(struct.unpack('<4I', bytes.fromhex(text)))
