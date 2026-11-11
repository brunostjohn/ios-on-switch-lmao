#!/usr/bin/env python3
"""The codegen gate: two directcheck dumps' fragment programs run on the same inputs, outputs compared bit for bit.

usage: gate.py <before dump> <after dump> --corpus <R lines>... [--invocations N] [--seed S] [--after-variant N]
               [--zero bank:first-last]...

Every state of the corpora in both dumps runs N quads of each program (quad.py over interp.py's lanes) on the same
seeded attributes, constant buffers, texture and storage-image results (specials included: NaNs with payloads,
infinities, signed zeros, f32 denormals, values past half's range). A state passes when every lane kills alike and makes
the same storage stores, and every lane that is not killed writes the same output registers (a discarded pixel's
registers are written nowhere), compared as the codes GM20B's store makes of them where the state's attachment is a
unorm format (rop.py); it is unsupported when either program uses an instruction the interpreter does not run.
"""

import argparse
import hashlib
import json
import random
import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import isa, nakstat, rop
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import LANES, run_quad

SPECIALS = [
    0x7FC00000,
    0x7FC12345,
    0xFFC00001,
    0x7F800000,
    0xFF800000,
    0x80000000,
    0x00000001,
    0x807FFFFF,
    0x477FF000,
    0x47800000,
    0xC7800000,
    0x3F800000,
    0x00800000,
    0x387FFFFF,
]
OMAP_TARGET_BYTE = 576 // 8


def word(rng: random.Random, nice: float) -> int:
    r = rng.random()
    if r < nice:
        return struct.unpack('<I', struct.pack('<f', rng.uniform(-0.25, 1.25)))[0]
    if r < nice + 0.15:
        return rng.choice(SPECIALS)
    if r < nice + 0.2:
        return rng.randrange(0, 64) * 4
    return rng.getrandbits(32)


class Seeded:
    """Inputs as pure functions of (seed, invocation, operands): the same for both programs."""

    def __init__(self, seed: int, invocation: int, pinned: dict | None = None):
        self.base = (seed, invocation)
        self.pinned = pinned or {}

    def rng(self, *key: object) -> random.Random:
        # repr, not hash(): Python salts a str's hash per process, which would make a quad's number name other inputs
        # in every run
        return random.Random(int.from_bytes(hashlib.blake2b(repr((self.base, key)).encode(), digest_size=8).digest()))

    def inputs(self, lane: int = 0, bound_cbuf: int = -1) -> Inputs:
        def attribute(addr: int) -> int:
            return 0x3F800000 if addr == 0x7C else word(self.rng('a', addr, lane), 0.9)

        def cbuf(bank: int, offset: int) -> int:
            if (bank, offset) in self.pinned:
                return self.pinned[bank, offset]
            return word(self.rng('c', bank, offset & 0xFFFFFFFF), 0.6)

        def texture(operands: tuple) -> list[int]:
            r = self.rng('t', operands)
            return [word(r, 0.85) for _ in range(4)]

        return Inputs(attribute, cbuf, texture, bound_cbuf)


def outputs(image: bytes) -> int:
    return struct.unpack_from('<I', image, OMAP_TARGET_BYTE)[0].bit_count()


def lane_result(result: tuple, widths: list) -> tuple:
    regs, killed, stores = result
    if killed:
        return killed, stores
    return [rop.store_code(v, w) if w else v for v, w in zip(regs, widths, strict=False)], killed, stores


def compare(  # noqa: PLR0913, PLR0917  the inputs' seed and pins and the state's attachments
    before: tuple, after: tuple, n: int, seed: int, pinned: dict | None = None, key: str = ''
) -> str:
    """Compare two programs, each (image, the constant buffer its bound textures read handles from or -1)."""
    count = outputs(before[0])
    if count != outputs(after[0]):
        return 'different: output maps'
    widths = rop.widths(key, struct.unpack_from('<I', before[0], OMAP_TARGET_BYTE)[0])

    def run(program: tuple, i: int) -> list:
        quad = [Seeded(seed, i, pinned).inputs(lane, program[1]) for lane in range(LANES)]
        return [lane_result(r, widths) for r in run_quad(program[0][isa.HEADER_BYTES :], quad, count)]

    for i in range(n):
        try:
            a, b = run(before, i), run(after, i)
        except UnsupportedError as e:
            return f'unsupported: {e}'
        if a != b:
            lane = next(k for k in range(LANES) if a[k] != b[k])
            return f'different: quad {i} lane {lane}: {describe(a[lane])} vs {describe(b[lane])}'
    return 'identical'


def describe(result: tuple) -> str:
    return 'killed' if result[0] is True else str([hex(x) for x in result[0]])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('before', type=Path)
    ap.add_argument('after', type=Path)
    ap.add_argument('--corpus', type=Path, action='append', default=[])
    ap.add_argument('--invocations', type=int, default=200)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--after-variant', type=int, help="compare against the after dump's fragment variant N")
    ap.add_argument('--zero', action='append', default=[], help='bank:first-last, constant-buffer bytes read as 0')
    ap.add_argument(
        '--zero-residuals', action='store_true', help="read the after dump's uniform bind residual push words as 0"
    )
    a = ap.parse_args()

    residuals = {}

    def load(d: Path, variant: int | None = None) -> dict:
        out = {}
        for f in d.glob('*.json'):
            j = json.loads(f.read_text())
            residuals[j['key']] = {(0, offset): 0 for offset in j.get('residual_words', [])}
            bound = j.get('fs_bound_textures', 0) - 1
            if variant is None:
                out[j['key']] = (bytes.fromhex(j['fs']), bound)
            elif (v := nakstat.fragment_variant(j, variant)) is not None:
                out[j['key']] = (bytes.fromhex(v['code']), v.get('bound_textures', 0) - 1)
        return out

    pinned = {}
    for z in a.zero:
        bank, span = z.split(':')
        first, last = (int(x, 0) for x in span.split('-'))
        pinned.update({(int(bank), offset): 0 for offset in range(first, last + 1, 4)})
    before, after = load(a.before), load(a.after, a.after_variant)
    failed = False
    for label, key in nakstat.labelled(a.corpus):
        if key not in before or key not in after:
            print(f'{label}: missing from a dump')
            continue
        pins = pinned | residuals[key] if a.zero_residuals else pinned
        verdict = compare(before[key], after[key], a.invocations, a.seed, pins, key)
        failed |= verdict.startswith('different')
        print(f'{label}: {verdict}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
