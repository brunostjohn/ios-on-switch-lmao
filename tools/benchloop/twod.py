"""The 2D engine: fills, scaled copies and resolves, one operation a command buffer, and their compare forms.

Fills come in the destination's own colour format (one packed word, as nouveau's EXA fill sends it), as nouveau's
reference fill (SRCCOPY, clip at the surface), clipped to a rectangle, unbracketed (no 3D waits around it), and last
after a clear-only pass in the same command buffer, the slot the 11 Nov 15:30 context-switch timeout ran in, in the
RF32 x4 float form that was running then. Scales and resolves read a source of the target's own format (no conversion
on the way in) whose 8-bit 2x2 sums are never 2 mod 4, so no equal-weight mean lands on a .5 tie unless a cell asks for
one. Each operation's compare form runs the same work through the 3D path or the copy engine; analyse() reads both.
"""

import random
import struct

from tools.benchloop import columns, protocol, texels

SIDE = 16
FILL_BYTE = 0xA5
RGBA = (0.2, 0.4, 0.6, 1.0)
F32 = 0xC0
# solid-prim colour format and packed colour per destination (cl902d.h SET_RENDER_SOLID_PRIM_COLOR_FORMAT)
OWN = {
    'bgra8unorm': (0xCF, 0xFF336699),
    'rgba8unorm': (0xD5, 0xFF996633),
    'bgra8unorm_srgb': (0xCF, 0xFF336699),
    'rgba8unorm_srgb': (0xD5, 0xFF996633),
    'rgb10a2unorm': (0xD1, 0xC0000000 | 0x266 << 20 | 0x199 << 10 | 0x0CC),
    'r8unorm': (0xF3, 0x33),
    'r16unorm': (0xEE, 0x3333),
    'rgba16float': (0xCA, struct.unpack('<I', struct.pack('<2e', 0.2, 0.4))[0]),
}
SCALE_FORMATS = ('bgra8unorm', 'rgba8unorm', 'rgb10a2unorm', 'rgba16float', 'rgba32float')
# the formats the pack has a sampled-quad state for, the 3D side of a bilinear scale (protocol.MIP_FORMATS)
SAMPLED3D_FORMATS = ('rgb10a2unorm', 'rgba16unorm', 'rgba32float')
CLIP = [4, 4, 12, 10]
RESOLVE_FORMATS = ('bgra8unorm', 'rgba8unorm', 'rgb10a2unorm', 'rgba16float')
# name: (du as a fraction, destination side)
SCALES = {'1to1': ((1, 1), 64), '2to1': ((2, 1), 32), '3to2': ((3, 2), 42), '3to7': ((3, 7), 140)}
TIMED = (256, 1024, 2048)


def f32_words(colour: tuple[float, ...]) -> list[int]:
    return [struct.unpack('<I', struct.pack('<f', v))[0] for v in colour]


def source_texels(seed: int = 11, side: int = 64) -> bytes:
    """Give a BGRA8 source whose channels are 4k + 1 at even (x, y) and 4k elsewhere: a 2x2 block sums to 1 mod 4."""
    rng = random.Random(seed)
    out = bytearray()
    for y in range(side):
        for x in range(side):
            bump = 1 if x % 2 == 0 and y % 2 == 0 else 0
            out += bytes(min(255, 4 * rng.randrange(64) + bump) for _ in range(3)) + bytes([255])
    return bytes(out)


def source(fmt: str = 'bgra8unorm', side: int = 64) -> dict:
    """Give the tie-free source in `fmt`: the same colours, stored as that format stores them."""
    data = source_texels(side=side)
    if fmt != 'bgra8unorm':
        colours = (texels.decode(data[i : i + 4], 'bgra8unorm') for i in range(0, len(data), 4))
        data = b''.join(texels.encode(c, fmt) for c in colours)
    return {'format': fmt, 'width': side, 'height': side, 'data': protocol.b64(data)}


def _experiment(eid: str, fmt: str, size: tuple[int, int], op: dict, **extra: object) -> dict:
    w, h = size
    e = {
        'id': eid,
        'kind': 'twod',
        'target': {'format': fmt, 'width': w, 'height': h},
        'twod': op,
        'readback': 'full',
        'fill': protocol.b64(bytes([FILL_BYTE]) * (w * h * protocol.FORMATS[fmt][1])),
        'meta': {'family': 'twod', 'op': op['op']},
    }
    return e | extra


def fill(eid: str, fmt: str, colour_format: int, colour: list[int], **op: object) -> dict:
    twod = {'op': 'fill', 'colorFormat': colour_format, 'color': colour} | op
    return _experiment(eid, fmt, (SIDE, SIDE), twod, rect=[1, 1, SIDE - 1, SIDE - 1])


def own_fill(fmt: str, suffix: str = '', **op: object) -> dict:
    colour_format, packed = OWN[fmt]
    return fill(f'fill-own{suffix}-{fmt}', fmt, colour_format, [packed, 0, 0, 0], **op)


def scale(name: str, fmt: str, bilinear: bool, corner: bool, **extra: object) -> dict:
    (num, den), side = SCALES[name]
    step = (num << 32) // den
    filter_ = 'bil' if bilinear else 'point'
    op = {'op': 'scale', 'du': step, 'dv': step, 'bilinear': bilinear, 'corner': corner}
    e = _experiment(f'scale-{name}-{filter_}-{"corner" if corner else "center"}-{fmt}', fmt, (side, side), op)
    return e | {'source': source(fmt)} | extra


def resolve(fmt: str, samples: int) -> dict:
    size = (32, 32) if samples == 4 else (32, 64)
    op = {'op': 'resolve', 'samples': samples}
    return _experiment(f'resolve-{samples}x-{fmt}', fmt, size, op, source=source(fmt), compare='resolve3d')


def sampled(name: str, fmt: str, production: bool = False) -> dict:
    """Give a whole-target bilinear scale mapping pixel centres against the pack's sampled-quad state.

    The centre origin, or the production form: the corner origin offset by half a step.
    """
    _, side = SCALES[name]
    step = (64 << 32) // side
    op = {'op': 'scale', 'du': step, 'dv': step, 'bilinear': True}
    if production:
        op |= {'corner': True, 'sx': step // 2, 'sy': step // 2}
    eid = f'scale-vs3d{"-prod" if production else ""}-{name}-{fmt}'
    return _experiment(eid, fmt, (side, side), op, source=source(fmt), compare='sampled3d')


def production(name: str, fmt: str) -> dict:
    """Give a scale in the form the driver's 2D path uses: the corner origin offset by half a step (probeResolveOp)."""
    e = scale(name, fmt, True, True)
    e['twod'] |= {'sx': e['twod']['du'] // 2, 'sy': e['twod']['dv'] // 2}
    return e | {'id': e['id'].replace('-corner-', '-prod-')}


def compare_fill(fmt: str) -> dict:
    """Give the own-format fill against the 3D path: a program writing the same colour over the same rect."""
    e = own_fill(fmt, '-vs3d')
    return e | {'compare': 'program', 'program': columns.program([RGBA] * SIDE)}


def timed(e: dict) -> dict:
    """Give `e` whole-target, read as a summary, five times after two warm-ups."""
    w, h = e['target']['width'], e['target']['height']
    out = {k: v for k, v in e.items() if k not in ('fill', 'rect')}
    return out | {'id': 't-' + e['id'], 'rect': [0, 0, w, h], 'readback': 'summary', 'repeats': 5, 'warmup': 2}


def generate() -> list[dict]:
    out = [own_fill(fmt) for fmt in OWN]
    out += [own_fill(fmt, '-ref', reference=True) for fmt in ('bgra8unorm', 'rgba8unorm', 'rgba16float')]
    out += [fill(f'fill-xcf-{fmt}', fmt, 0xCF, [OWN['bgra8unorm'][1], 0, 0, 0]) for fmt in ('rgba8unorm', 'r8unorm')]
    out += [compare_fill(fmt) for fmt in ('bgra8unorm', 'rgba8unorm')]
    copy = scale('1to1', 'bgra8unorm', False, True, compare='copy')
    out.append(copy | {'id': copy['id'].replace('scale-', 'scale-vsce-')})
    for fmt in SCALE_FORMATS:
        out += [scale(name, fmt, True, corner) for name in ('2to1', '3to2', '3to7') for corner in (True, False)]
        out += [scale('2to1', fmt, False, True), scale('1to1', fmt, False, True)]
    out += [resolve(fmt, samples) for fmt in RESOLVE_FORMATS for samples in (4, 2)]
    out += [production(name, fmt) for fmt in SCALE_FORMATS for name in ('2to1', '3to2', '3to7')]
    # 1:1 is the control: both paths copy texel centres, so its histogram must be all 0
    for fmt in SAMPLED3D_FORMATS:
        out += [sampled(name, fmt) for name in ('1to1', '2to1', '3to2', '3to7')]
        out += [sampled(name, fmt, production=True) for name in ('2to1', '3to2', '3to7')]
    out.append(own_fill('bgra8unorm', '-clip', clip=CLIP))
    clipped = scale('1to1', 'bgra8unorm', False, True)
    clipped['twod'] |= {'clip': CLIP}
    out.append(clipped | {'id': clipped['id'].replace('scale-', 'scale-clip-')})
    for side in TIMED:
        for fmt in ('bgra8unorm', 'rgba16float'):
            colour_format, packed = OWN[fmt]
            e = _experiment(f'fill-{fmt}-{side}', fmt, (side, side), {'op': 'fill', 'colorFormat': colour_format})
            out.append(timed(e | {'twod': e['twod'] | {'color': [packed, 0, 0, 0]}}))
        up = {'op': 'scale', 'du': (62 << 32) // (side - 2), 'dv': (62 << 32) // (side - 2), 'bilinear': True}
        out.append(timed(_experiment(f'upscale-{side}', 'bgra8unorm', (side, side), up)))
    out.append(own_fill('bgra8unorm', '-unbracketed', bracketed=False))
    out += [fill(f'fill-f32-{fmt}', fmt, F32, f32_words(RGBA)) for fmt in ('rgba32float', 'rgba16float', 'bgra8unorm')]
    out.append(fill('fill-f32-unbracketed-bgra8unorm', 'bgra8unorm', F32, f32_words(RGBA), bracketed=False))
    return out + hang_slot()


def hang_slot() -> list[dict]:
    """Give the 15:30 timeout's slot: a clear-only pass, then the fill, bracketed and not; f32 unbracketed last."""
    words = f32_words(RGBA)
    return [
        own_fill('bgra8unorm', '-afterclear', clearFirst=True),
        own_fill('bgra8unorm', '-afterclear-unbracketed', clearFirst=True, bracketed=False),
        fill('fill-f32-afterclear-bgra8unorm', 'bgra8unorm', F32, words, clearFirst=True),
        fill('fill-f32-afterclear-unbracketed-bgra8unorm', 'bgra8unorm', F32, words, clearFirst=True, bracketed=False),
    ]


def _texel_data(result: dict, key: str = 'texels') -> bytes:
    section = result.get(key) or {}
    if key == 'compare':
        section = section.get('texels') or {}
    return protocol.unb64(section.get('data') or '')


def wanted_fill(e: dict) -> bytes | None:
    """Give the texel a fill should write: its colour in the destination's format, where the host can say."""
    fmt, t = e['target']['format'], e['twod']
    if t['colorFormat'] == F32 and fmt in texels.UNORM_BITS | dict.fromkeys(('rgba16float', 'rgba32float')):
        return texels.encode(RGBA, fmt, 'gm20b') if fmt in texels.UNORM_BITS else None
    if t['colorFormat'] == OWN.get(fmt, (None,))[0] and protocol.FORMATS[fmt][1] <= 4:
        return struct.pack('<I', t['color'][0])[: protocol.FORMATS[fmt][1]]
    return None


def written_box(e: dict) -> tuple[int, int, int, int]:
    """Give the texels the operation writes: its rect, cut to its clip rectangle where it has one."""
    x0, y0, x1, y1 = protocol.rect_of(e, protocol.target_of(e))
    cx0, cy0, cx1, cy1 = e['twod'].get('clip', [x0, y0, x1, y1])
    return max(x0, cx0), max(y0, cy0), min(x1, cx1), min(y1, cy1)


def background(e: dict) -> bytes:
    """Give what a texel the operation does not write holds: the fill, or the clear colour after a clear-only pass."""
    fmt = e['target']['format']
    if e['twod'].get('clearFirst'):
        return texels.encode(tuple(e.get('clear', [0.0, 0.0, 0.0, 0.0])), fmt)
    return bytes([FILL_BYTE]) * protocol.FORMATS[fmt][1]


def analyse(experiments: list[dict], results: dict[str, dict]) -> list[dict]:
    """Give, per operation: whether it ran, its form, whether it kept to what it writes, and its compare histogram."""
    rows = []
    for e in experiments:
        result = results.get(e['id']) or {}
        data = _texel_data(result)
        fmt = e['target']['format']
        bpp, width = protocol.FORMATS[fmt][1], e['target']['width']
        x0, y0, x1, y1 = written_box(e)
        cells = [data[i : i + bpp] for i in range(0, len(data), bpp)]
        written = [x0 <= k % width < x1 and y0 <= k // width < y1 for k in range(len(cells))]
        inside = [c for c, w in zip(cells, written, strict=True) if w]
        outside = [c for c, w in zip(cells, written, strict=True) if not w]
        row = {'id': e['id'], 'status': result.get('status'), 'why': result.get('why', ''), 'form': result.get('form')}
        if data and ('fill' in e or e['twod'].get('clearFirst')):
            row['kept_outside'] = all(c == background(e) for c in outside)
        if data and e['twod']['op'] == 'fill' and (want := wanted_fill(e)) is not None:
            row['fill_exact'] = sum(c == want for c in inside)
            row['fill_cells'] = len(inside)
        if 'compare' in e:
            compare = result.get('compare') or {}
            row['compare'] = {'status': compare.get('status'), 'histogram': compare.get('histogram')}
        rows.append(row)
    return rows
