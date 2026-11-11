"""SR_AFFINITY in every pixel of a full-screen draw: the GPC index NAK's pixel interlock takes from its low byte.

NAK's lowering puts a warp's ticket word at (affinity & 0xff) * 256 + ticket id, and the raw path's ticket buffer is
4 KiB, room for affinities 0..3. Each pixel writes the whole register; the summary's distinct texels are the histogram.
"""

import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, latency, program, sph
from tools.benchloop.sm53.memops import SR_AFFINITY, SR_VIRTID, s2r

TICKET_BUFFER_KIB = 4
VALUE, OTHER = 4, 5
# id: (target width, height, readback, second register's special register)
SHAPES = {
    'affinity.screen': (1280, 720, 'summary', SR_AFFINITY),
    'affinity.tile': (256, 128, 'full', SR_AFFINITY),
    'affinity.screen4': (1280, 720, 'summary', SR_AFFINITY),
    'affinity.virtid': (256, 128, 'full', SR_VIRTID),
}


def code(other: int) -> bytes:
    body = [
        s2r(VALUE, SR_AFFINITY).sched(wr_bar=0, delay=1),
        s2r(OTHER, other).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
    ]
    return e.bundle(e.finish([*body, *program.epilogue((VALUE, OTHER))]))


def experiment(eid: str) -> dict:
    width, height, readback, other = SHAPES[eid]
    c = code(other)
    x = {
        'id': eid,
        'kind': 'fragment',
        'target': {'format': 'rgba32float', 'width': width, 'height': height},
        'draws': [4] if eid.endswith('4') else [1],
        'readback': readback,
        'program': {'sph': protocol.b64(sph.pixel()), 'code': protocol.b64(c), 'data': '', 'gprs': OTHER + 1},
        'meta': {'family': 'affinity', 'second': 'virtid' if other == SR_VIRTID else 'affinity'},
    }
    protocol.validate_experiment(x)
    return x


def generate() -> list[dict]:
    return [latency.warm_up('affinity.warm'), *(experiment(eid) for eid in SHAPES)]


def _counts(result: dict) -> tuple[dict[tuple[int, int], int], int]:
    """Give (affinity, second register) -> pixels, and the pixels that came back."""
    texels = result.get('texels') or {}
    data = protocol.unb64(texels.get('data') or '')
    out: dict[tuple[int, int], int] = {}
    if data:
        for k in range(0, len(data), 16):
            key = program.unpack(list(struct.unpack_from('<4I', data, k)))
            out[key] = out.get(key, 0) + 1
    else:
        for d in texels.get('distinct') or []:
            key = program.unpack(program.texel_words(d['value']))
            out[key] = out.get(key, 0) + d['count']
    return out, sum(out.values())


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give per experiment the pixels by affinity byte, the largest, and the ticket buffer it needs."""
    out = {}
    for x in experiments:
        if x['meta'].get('family') != 'affinity':
            continue
        r = results.get(x['id']) or {}
        if r.get('status') != 'ok':
            out[x['id']] = {'status': r.get('status', 'missing'), 'why': r.get('why', '')}
            continue
        counts, seen = _counts(r)
        by_byte: dict[int, int] = {}
        for (value, _), n in counts.items():
            by_byte[value & 0xFF] = by_byte.get(value & 0xFF, 0) + n
        top = max(by_byte, default=None)
        out[x['id']] = {
            'status': 'ok',
            'pixels': x['target']['width'] * x['target']['height'],
            'counted': seen,
            'histogram': dict(sorted(by_byte.items())),
            'whole_values': sorted({f'{v:#010x}' for v, _ in counts})[:64],
            'max': top,
            'ticket_buffer_kib': None if top is None else max(TICKET_BUFFER_KIB, top + 1),
            'fits': top is not None and top < TICKET_BUFFER_KIB,
        }
        if x['meta']['second'] == 'virtid':
            out[x['id']]['virtid'] = len({o for _, o in counts})
    return out
