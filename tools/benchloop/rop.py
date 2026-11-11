"""How the ROP stores a float in a unorm channel.

Values on both sides of every rounding boundary written unblended,
read back, and held to each rounding model of texels.ROUNDING. The models that match every texel are the answer.
"""

import math
import struct

from tools.benchloop import columns, protocol, texels

FRACTIONS = (0.0, 0.25, 0.4999, 0.5, 0.5001, 0.75, 0.9999)
SPECIALS = (-1.0, -0.0, 1.0, 1.0001, 2.0, math.inf, -math.inf, math.nan, 1e-45, 2.0**-126)


def nudged(x: float, ulps: int) -> float:
    bits = struct.unpack('<I', struct.pack('<f', x))[0] + ulps
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def values(bits: int) -> list[float]:
    top = (1 << bits) - 1
    out = list(SPECIALS)
    for code in range(0, top, max(1, top // 63)):
        for t in FRACTIONS:
            v = texels.f32((code + t) / top)
            out += [v, nudged(v, -1), nudged(v, 1)] if t == 0.5 else [v]
    return out


def generate(formats: tuple[str, ...] = ('bgra8unorm', 'rgba8unorm', 'rgb10a2unorm')) -> list[dict]:
    out = []
    for fmt in formats:
        vs = values(texels.UNORM_BITS[fmt][0])
        vs += [0.0] * (-len(vs) % 4)
        lanes = [tuple(vs[i : i + 4]) for i in range(0, len(vs), 4)]
        for part in range(0, len(lanes), 1024):
            chunk = lanes[part : part + 1024]
            out.append(
                {
                    'id': f'rop-{fmt}-{part // 1024}',
                    'kind': 'readback',
                    'target': {'format': fmt, 'width': len(chunk), 'height': 1},
                    'program': columns.program(chunk),
                    'readback': 'full',
                    'meta': {'family': 'rop', 'colours': chunk},
                }
            )
    return out


def check(experiment: dict, result: dict) -> dict:
    fmt = experiment['target']['format']
    bits = texels.UNORM_BITS[fmt]
    row = {'id': experiment['id'], 'status': result.get('status'), 'checked': 0, 'models': {}}
    data = (result.get('texels') or {}).get('data')
    if result.get('status') != 'ok' or not data:
        return row | {'why': result.get('why', 'no texels came back')}
    matches = dict.fromkeys(texels.ROUNDING, 0)
    first_miss: dict[str, dict] = {}
    for x, texel in enumerate(texels.split(protocol.unb64(data), fmt)):
        got = texels.codes(texel, fmt)
        for lane, value in enumerate(experiment['meta']['colours'][x]):
            row['checked'] += 1
            for model in texels.ROUNDING:
                want = texels.unorm(value, bits[lane], model)
                if want == got[lane]:
                    matches[model] += 1
                elif model not in first_miss:
                    first_miss[model] = {'value': repr(value), 'got': got[lane], 'want': want}
    row['models'] = {m: {'matched': n, 'first_miss': first_miss.get(m)} for m, n in matches.items()}
    row['verdict'] = [m for m, n in matches.items() if n == row['checked']]
    return row
