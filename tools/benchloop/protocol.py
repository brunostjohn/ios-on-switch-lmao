"""The batch and result files the host and HW Test's bench loop exchange (benchloop/2).

A batch is one JSON object; each experiment in it is validated here before it is written, and again by the app
(hw_test_app/bench) before it runs. Byte strings travel as base64. Limits keep every command buffer far below the
kernel's stuck-channel rule (500 ms without a completed fence), which bounds a hung experiment instead of a CPU timeout.
"""

import base64
import json
import re
from dataclasses import dataclass

PROTOCOL = 'benchloop/2'
NAME = re.compile(r'^[a-z0-9][a-z0-9_.-]{0,63}$')
KINDS = ('noop', 'known', 'fragment', 'compute', 'readback', 'twod')
TEMPLATE = re.compile(r'^[0-9a-f]{16}$')
READBACK = ('none', 'summary', 'full')

MAX_EXPERIMENTS = 256
MAX_SIDE = 2048
MAX_DRAW_COUNTS = 16
MAX_DRAWS = 256
MAX_REPEATS = 64
MAX_WARMUP = 16
MAX_COMMAND_BUFFERS = 1024
MAX_CODE = 64 * 1024
MAX_DATA = 64 * 1024
MAX_METHOD_PAIRS = 1024
MAX_GROUPS = 65535
MAX_THREADS = 1024
SPH_BYTES = 80
# Estimated GPU time of one command buffer, at the SMs' 256 lanes a clock and 768 MHz
MAX_COMMAND_BUFFER_MS = 50.0
CLOCK_HZ = 768e6
LANES_PER_CLOCK = 256

# MTLPixelFormat raw values the raw path renders (raw_gpu.cpp kFormats with a render target format), bytes per texel
FORMATS = {
    'r32float': (55, 4),
    'rgba8unorm': (70, 4),
    'bgra8unorm': (80, 4),
    'rgb10a2unorm': (90, 4),
    'rg32float': (105, 8),
    'rgba16float': (115, 8),
    'rgba32float': (125, 16),
    'r8unorm': (10, 1),
    'r16unorm': (20, 2),
    'r16float': (25, 2),
    'rg8unorm': (30, 2),
    'b5g6r5unorm': (40, 2),
    'bgr5a1unorm': (43, 2),
    'rg16unorm': (60, 4),
    'rg16float': (65, 4),
    'rgba8unorm_srgb': (71, 4),
    'bgra8unorm_srgb': (81, 4),
    'rg11b10float': (92, 4),
    'bgr10a2unorm': (94, 4),
    'rgba16unorm': (110, 8),
}

# MAXWELL_B methods a readback experiment may set around its draw: the blend, iterated blend and colour write state, and
# every ROP option that could trade colour precision (the owner's question: is an inexact result our configuration).
# The raw path owns everything else (raw_owned.h); hw_test_app/bench/switch_bench_methods.h holds the same ranges.
METHOD_RANGES = (
    (0x02E0, 0x02E4),  # DECOMPRESS_SURFACE
    (0x02F4, 0x02F8),  # SET_ITERATED_BLEND_OPTIMIZATION
    (0x0754, 0x0758),  # SET_HYBRID_ANTI_ALIAS_CONTROL
    (0x0D9C, 0x0DA0),  # SET_REDUCE_COLOR_THRESHOLDS_ENABLE
    (0x0DD0, 0x0DD8),  # SET_ITERATED_BLEND, SET_ITERATED_BLEND_PASS
    (0x0DF0, 0x0DF8),  # LOAD_ITERATED_BLEND_INSTRUCTION_POINTER, LOAD_ITERATED_BLEND_INSTRUCTION
    (0x0F40, 0x0F54),  # SET_CONSTANT_COLOR_RENDERING, _RED.._ALPHA
    (0x0F90, 0x0F94),  # SET_SINGLE_CT_WRITE_CONTROL
    (0x0FAC, 0x0FB0),  # SET_CT_MRT_ENABLE
    (0x0FDC, 0x0FE0),  # SET_BLEND_OPT_CONTROL
    (0x10CC, 0x10D0),  # SET_REDUCE_COLOR_THRESHOLDS_UNORM8
    (0x10E0, 0x10F4),  # SET_REDUCE_COLOR_THRESHOLDS_UNORM10..SRGB8
    (0x113C, 0x1140),  # SET_FILL_VIA_TRIANGLE
    (0x1140, 0x1144),  # SET_BLEND_PER_FORMAT_ENABLE
    (0x1220, 0x1224),  # SET_COMPRESSION_THRESHOLD
    (0x12E0, 0x12E8),  # SET_ALPHA_TO_COVERAGE_DITHER_CONTROL, SET_BLEND_STATE_PER_TARGET
    (0x131C, 0x132C),  # SET_BLEND_CONST_RED..ALPHA
    (0x133C, 0x1360),  # SET_BLEND_SEPARATE_FOR_ALPHA..SET_SINGLE_ROP_CONTROL
    (0x1360, 0x1380),  # SET_BLEND(i)
    (0x13A8, 0x13AC),  # SET_PS_SATURATE
    (0x1480, 0x1500),  # SET_ITERATED_BLEND_CONSTANT_RED/GREEN/BLUE(j)
    (0x15B8, 0x15BC),  # SET_SRGB_WRITE
    (0x19C0, 0x19C4),  # SET_BLEND_FLOAT_OPTION
    (0x1A00, 0x1A20),  # SET_CT_WRITE(i)
    (0x1E00, 0x1F00),  # SET_BLEND_PER_TARGET_*(j)
    (0x2600, 0x2604),  # SET_COLOR_CLAMP
)


# Every field an experiment and its parts may carry: the app refuses any other, so a misspelt field is an invalid
# experiment rather than a different one
EXPERIMENT_KEYS = {
    'id',
    'kind',
    'target',
    'rect',
    'draws',
    'repeats',
    'warmup',
    'clear',
    'readback',
    'template',
    'fill',
}
EXPERIMENT_KEYS |= {'program', 'compute', 'twod', 'methods', 'restore', 'targets', 'source', 'compare', 'compressed'}
EXPERIMENT_KEYS |= {'meta', 'expect'}
TARGET_KEYS = {'format', 'width', 'height'}
PROGRAM_KEYS = {'code', 'data', 'sph', 'gprs'}
COMPUTE_KEYS = {'code', 'data', 'gprs', 'threads', 'groups', 'output', 'chain', 'schedule'}
TWOD_KEYS = {'op', 'fmt2d', 'colorFormat', 'color', 'du', 'dv', 'sx', 'sy', 'bilinear', 'corner'}
TWOD_KEYS |= {'reference', 'bracketed', 'samples', 'clip', 'clearFirst'}
SOURCE_KEYS = {'format', 'width', 'height', 'data'}
TWOD_OPS = ('fill', 'scale', 'resolve')
# Target formats the 2D engine names a surface format for (raw_probe_ops.h probeTwodFormat), by MTLPixelFormat
TWOD_FORMATS = {10, 20, 25, 30, 60, 65, 70, 71, 80, 81, 90, 92, 110, 115, 125}
# Target formats the pack has a sampled-quad state for (raw_mip.h mipState): the 3D side of a bilinear scale's compare
MIP_FORMATS = {1, 20, 25, 40, 43, 55, 60, 65, 71, 81, 90, 92, 94, 105, 110, 125}
# A multisampled image's samples across and down, log2 (fx_image.cpp msLayout)
SAMPLE_GRIDS = {2: (1, 0), 4: (1, 1)}
ONE = 1 << 32
# What a scale, resolve or copy reads when the experiment gives no source: the app's own picture
PICTURE = ('bgra8unorm', 64, 64)
# compare: the same operation again through another unit, both results and their difference returned
COMPARES = {'twod': ('program', 'copy', 'resolve3d', 'sampled3d'), 'readback': ('plain',)}
MAX_OUTPUT = 64 * 1024
# QMDs a compute experiment's own one may name dependent, one after another (hw_test_app/bench switch_bench.h)
MAX_CHAIN = 3


class InvalidError(ValueError):
    pass


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def unb64(text: str) -> bytes:
    return base64.b64decode(text.encode(), validate=True)


def method_allowed(method: int) -> bool:
    return method % 4 == 0 and any(lo <= method < hi for lo, hi in METHOD_RANGES)


def _require(ok: bool, why: str) -> None:
    if not ok:
        raise InvalidError(why)


def _keys(value: dict, allowed: set[str], what: str) -> None:
    unknown = sorted(set(value) - allowed)
    _require(not unknown, f'{what}: unknown field {", ".join(unknown)}')


def _flag(value: object, what: str) -> bool:
    _require(isinstance(value, bool), f'{what} is true or false')
    return value


def _count(value: object, lo: int, hi: int, what: str) -> int:
    _require(isinstance(value, int) and not isinstance(value, bool) and lo <= value <= hi, f'{what} not in {lo}..{hi}')
    return value


def _pairs(value: object, what: str) -> list[list[int]]:
    _require(isinstance(value, list) and len(value) <= MAX_METHOD_PAIRS, f'{what}: at most {MAX_METHOD_PAIRS} pairs')
    for pair in value:
        _require(isinstance(pair, list) and len(pair) == 2, f'{what}: [method, value] pairs')
        _count(pair[1], 0, 0xFFFFFFFF, f'{what} value')
        _require(isinstance(pair[0], int) and method_allowed(pair[0]), f'{what}: method {pair[0]:#x} is not allowed')
    return value


@dataclass(frozen=True)
class Target:
    format: str
    width: int
    height: int

    @property
    def texel_bytes(self) -> int:
        return FORMATS[self.format][1]


def target_of(experiment: dict) -> Target:
    t = experiment.get('target') or {}
    _require(isinstance(t, dict), 'target is an object')
    _keys(t, TARGET_KEYS, 'target')
    _require(t.get('format') in FORMATS, f'target format {t.get("format")!r} is not one the raw path renders')
    return Target(t['format'], _count(t.get('width'), 1, MAX_SIDE, 'width'), _count(t.get('height'), 1, MAX_SIDE, 'h'))


def rect_of(experiment: dict, target: Target) -> tuple[int, int, int, int]:
    rect = experiment.get('rect', [0, 0, target.width, target.height])
    _require(isinstance(rect, list) and len(rect) == 4, 'rect is [x0, y0, x1, y1]')
    x0, y0, x1, y1 = rect
    _require(0 <= x0 < x1 <= target.width and 0 <= y0 < y1 <= target.height, 'rect outside the target')
    return x0, y0, x1, y1


def instructions(code: bytes) -> int:
    return len(code) // 32 * 3


def estimated_ms(experiment: dict) -> float:
    """Give the GPU time of the experiment's longest command buffer if every lane ran every instruction at full rate."""
    if experiment['kind'] in ('noop', 'known') or (experiment['kind'] == 'twod' and 'program' not in experiment):
        return 0.0
    if experiment['kind'] == 'compute':
        c = experiment['compute']
        lanes = c['threads'][0] * c['threads'][1] * c['threads'][2] * c['groups'][0] * c['groups'][1] * c['groups'][2]
        work = lanes * instructions(unb64(c['code']))
    else:
        x0, y0, x1, y1 = rect_of(experiment, target_of(experiment))
        # a warp of 32 lanes runs every instruction however few of its lanes are covered
        work = max(32, (x1 - x0) * (y1 - y0)) * instructions(unb64(experiment['program']['code']))
    return max(experiment.get('draws', [1])) * work / LANES_PER_CLOCK / CLOCK_HZ * 1e3


def reads_position_w(sph: bytes) -> bool:
    return bool(int.from_bytes(sph[20:24], 'little') & 1 << 31)


def _program(program: object, field: str) -> None:
    _require(isinstance(program, dict), f'{field} is an object')
    _keys(program, COMPUTE_KEYS if field == 'compute' else PROGRAM_KEYS, field)
    code = unb64(program.get('code', ''))
    _require(0 < len(code) <= MAX_CODE and len(code) % 32 == 0, f'{field}.code: whole 32-byte bundles, at most 64 KiB')
    data = unb64(program.get('data', ''))
    _require(len(data) <= MAX_DATA and len(data) % 16 == 0, f'{field}.data: 16-byte multiples, at most 64 KiB')
    _count(program.get('gprs'), 1, 255, f'{field}.gprs')
    if field == 'program':
        sph = unb64(program.get('sph', ''))
        _require(len(sph) == SPH_BYTES, 'program.sph is 80 bytes')
        _require(
            reads_position_w(sph), 'program.sph: a pixel header must read position W (bit 191), or the GPU traps it'
        )


def source_of(experiment: dict) -> Target:
    """Give what a scale, resolve or copy reads: the experiment's source texture, else the app's picture."""
    src = experiment.get('source')
    if src is None:
        return Target(*PICTURE)
    _require(isinstance(src, dict), 'source is an object')
    _keys(src, SOURCE_KEYS, 'source')
    _require(src.get('format') in FORMATS, f'source format {src.get("format")!r} is not one the raw path renders')
    _require(FORMATS[src['format']][0] in TWOD_FORMATS, 'source format: the 2D engine has no surface format for it')
    width = _count(src.get('width'), 1, MAX_SIDE, 'source.width')
    t = Target(src['format'], width, _count(src.get('height'), 1, MAX_SIDE, 'source.height'))
    _require(len(unb64(src.get('data', ''))) == t.width * t.height * t.texel_bytes, 'source.data is the whole source')
    return t


def _twod(e: dict, target: Target) -> None:
    t = e.get('twod')
    _require(isinstance(t, dict) and t.get('op') in TWOD_OPS, f'twod is an object whose op is one of {TWOD_OPS}')
    _keys(t, TWOD_KEYS, 'twod')
    op = t['op']
    color = t.get('color', [0, 0, 0, 0])
    _require(isinstance(color, list) and len(color) == 4, 'twod.color is four words')
    for c in color:
        _count(c, 0, 0xFFFFFFFF, 'twod.color')
    for field in ('fmt2d', 'colorFormat'):
        _count(t.get(field, 0), 0, 255, f'twod.{field}')
    for field in ('du', 'dv', 'sx', 'sy'):
        _count(t.get(field, 0), 0, (1 << 63) - 1, f'twod.{field}')
    for field in ('bilinear', 'corner', 'bracketed', 'reference', 'clearFirst'):
        _flag(t.get(field, False), f'twod.{field}')
    _require(not t.get('reference') or op == 'fill', 'twod.reference is the reference fill: fill only')
    _require(op != 'resolve' or not ({'clip', 'clearFirst'} & set(t)), 'a resolve takes no clip and no clear first')
    if 'clip' in t:
        clip = t['clip']
        _require(isinstance(clip, list) and len(clip) == 4, 'twod.clip is [x0, y0, x1, y1]')
        x0, y0, x1, y1 = (_count(c, 0, MAX_SIDE, 'twod.clip') for c in clip)
        _require(x0 < x1 <= target.width and y0 < y1 <= target.height, 'twod.clip inside the target')
    _require(
        t.get('fmt2d', 0) or FORMATS[target.format][0] in TWOD_FORMATS,
        'target format: the 2D engine has no surface format for it (give twod.fmt2d)',
    )
    _require(op != 'fill' or 'source' not in e, 'a fill reads no source')
    source = source_of(e)
    if op == 'resolve':
        _require(t.get('samples') in SAMPLE_GRIDS, f'twod.samples is one of {sorted(SAMPLE_GRIDS)}')
        sx, sy = SAMPLE_GRIDS[t['samples']]
        _require(rect_of(e, target) == (0, 0, target.width, target.height), 'a resolve writes the whole target')
        _require(
            target.width << sx <= source.width and target.height << sy <= source.height,
            "a resolve's sample grid is copied from the source's corner: the source must cover it",
        )
    else:
        _require('samples' not in t, "twod.samples is a resolve's")


def centre_mapped(t: dict) -> bool:
    """Say whether a scale maps destination pixel centres onto the source, as the 3D path's sampled quad does.

    The centre origin with no offset, or the production form (raw_probe_ops.h probeResolveOp): the corner origin
    offset by half a step.
    """
    du, dv, sx, sy = (t.get(k, ONE if k in ('du', 'dv') else 0) for k in ('du', 'dv', 'sx', 'sy'))
    if t.get('corner'):
        return sx == du // 2 and sy == dv // 2
    return sx == 0 and sy == 0


def _compare(e: dict, target: Target) -> None:
    compare = e['compare']
    _require(compare in COMPARES.get(e['kind'], ()), f'compare: one of {COMPARES.get(e["kind"], ())} for {e["kind"]}')
    if e['kind'] == 'readback':
        _require(e.get('compressed') is True, 'compare plain is the compressed target against a plain one')
        return
    t = e['twod']
    if compare == 'program':
        _require(t['op'] in ('fill', 'scale'), 'compare program: a fill or scale against the program drawn over it')
        _program(e.get('program'), 'program')
    elif compare == 'copy':
        one_to_one = t.get('du', ONE) == ONE and t.get('dv', ONE) == ONE and not t.get('sx') and not t.get('sy')
        _require(t['op'] == 'scale' and one_to_one and not t.get('bilinear'), 'compare copy: a 1:1 point scale')
        _require(source_of(e).format == target.format, 'compare copy: the copy engine copies bytes, one format')
        x0, y0, x1, y1 = rect_of(e, target)
        source = source_of(e)
        _require(x1 - x0 <= source.width and y1 - y0 <= source.height, 'compare copy: the rect inside the source')
    elif compare == 'sampled3d':
        whole = rect_of(e, target) == (0, 0, target.width, target.height)
        _require(t['op'] == 'scale' and t.get('bilinear') and whole, 'compare sampled3d: a whole-target bilinear scale')
        _require(
            centre_mapped(t),
            'compare sampled3d: pixel centres mapped, the centre origin or the corner with half a step',
        )
        _require(
            FORMATS[target.format][0] in MIP_FORMATS, 'compare sampled3d: the pack has no sampled-quad state for it'
        )
    else:
        _require(t['op'] == 'resolve', "compare resolve3d: a resolve against the pack's resolve state")


def _compute(c: object) -> None:
    _program(c, 'compute')
    for field, hi in (('threads', MAX_THREADS), ('groups', MAX_GROUPS)):
        v = c.get(field)
        _require(isinstance(v, list) and len(v) == 3, f'compute.{field} is [x, y, z]')
        for n in v:
            _count(n, 1, hi, f'compute.{field}')
    _require(c['threads'][0] * c['threads'][1] * c['threads'][2] <= MAX_THREADS, 'compute.threads > 1024')
    if 'output' in c:
        output = _count(c['output'], 16, MAX_OUTPUT, 'compute.output')
        _require(output % 16 == 0, 'compute.output: 16-byte multiples')
        _require(
            len(unb64(c.get('data', ''))) >= 16, "compute.output: its address goes in the shader data's first 8 bytes"
        )


def _chain(c: dict) -> None:
    """Check a QMD chain: the kernels after the experiment's own, and per QMD before the last whether it schedules it."""
    chain = c.get('chain')
    _require(isinstance(chain, list) and 1 <= len(chain) <= MAX_CHAIN, f'compute.chain: 1..{MAX_CHAIN} kernels')
    _require('output' in c, "compute.chain: an output buffer for the QMDs' releases")
    for i, link in enumerate(chain):
        _program(link, f'compute.chain[{i}]')
        _require(len(unb64(link.get('data', ''))) >= 16, "compute.chain: each kernel's data holds two addresses")
    schedule = c.get('schedule')
    _require(isinstance(schedule, list) and len(schedule) == len(chain), 'compute.schedule: one flag per chain kernel')
    for flag in schedule:
        _require(isinstance(flag, bool), 'compute.schedule: true or false')


def _forms(e: dict, target: Target) -> None:
    """Check the source, compressed and compare fields: which kinds take them and what each needs."""
    if e['kind'] == 'twod':
        _twod(e, target)
    else:
        _require('source' not in e, 'only twod experiments read a source')
    if 'compressed' in e:
        _require(e['kind'] == 'readback' and _flag(e['compressed'], 'compressed'), 'compressed: a readback target')
    if 'compare' in e:
        _compare(e, target)
    elif e['kind'] == 'twod':
        _require('program' not in e, 'a twod experiment draws a program only to compare with it')


def validate_experiment(e: dict) -> None:
    _require(isinstance(e, dict), 'an experiment is an object')
    _keys(e, EXPERIMENT_KEYS, 'experiment')
    _require(isinstance(e.get('id'), str) and bool(NAME.match(e['id'])), f'id {e.get("id")!r}')
    _require(e.get('kind') in KINDS, f'{e["id"]}: kind {e.get("kind")!r}')
    target = target_of(e)
    rect_of(e, target)
    draws = e.get('draws', [1])
    _require(isinstance(draws, list) and 1 <= len(draws) <= MAX_DRAW_COUNTS, 'draws: 1..16 counts')
    for d in draws:
        _count(d, 0 if e['kind'] == 'noop' else 1, MAX_DRAWS, 'draws')
    repeats = _count(e.get('repeats', 1), 1, MAX_REPEATS, 'repeats')
    warmup = _count(e.get('warmup', 0), 0, MAX_WARMUP, 'warmup')
    _require(len(draws) * (repeats + warmup) <= MAX_COMMAND_BUFFERS, 'too many command buffers')
    clear = e.get('clear', [0, 0, 0, 0])
    _require(isinstance(clear, list) and len(clear) == 4, 'clear is four floats')
    _require(e.get('readback', 'summary') in READBACK, 'readback is none, summary or full')
    if 'template' in e:
        _require(
            isinstance(e['template'], str) and bool(TEMPLATE.match(e['template'])) and int(e['template'], 16) != 0,
            "template is a pack state's key hash, 16 hex digits",
        )
    if 'fill' in e:
        fill = unb64(e['fill'])
        _require(len(fill) == target.width * target.height * target.texel_bytes, 'fill is the whole target, packed')
    if e['kind'] in ('fragment', 'readback'):
        _program(e.get('program'), 'program')
    if e['kind'] == 'compute':
        _compute(e.get('compute'))
        if 'chain' in e['compute'] or 'schedule' in e['compute']:
            _chain(e['compute'])
            _require(draws == [1] and repeats == 1 and warmup == 0, 'compute.chain: one command buffer')
    _forms(e, target)
    if e['kind'] == 'readback':
        _pairs(e.get('methods', []), 'methods')
        _pairs(e.get('restore', []), 'restore')
        _require(e.get('targets', 0) in (0, 1), 'targets is 1 (colour 0 bound alone) or left out')
    else:
        _require('methods' not in e and 'restore' not in e, 'only readback experiments set methods')
        _require('targets' not in e, 'only readback experiments choose their targets')
    _require(
        estimated_ms(e) <= MAX_COMMAND_BUFFER_MS, f'{e["id"]}: a command buffer would take {estimated_ms(e):.1f} ms'
    )


def batch(name: str, experiments: list[dict]) -> dict:
    _require(bool(NAME.match(name)), f'batch name {name!r}')
    _require(0 < len(experiments) <= MAX_EXPERIMENTS, f'1..{MAX_EXPERIMENTS} experiments a batch')
    ids = [e.get('id') for e in experiments]
    _require(len(set(ids)) == len(ids), 'experiment ids repeat')
    for e in experiments:
        validate_experiment(e)
    return {'protocol': PROTOCOL, 'batch': name, 'experiments': experiments}


def wire(experiment: dict) -> dict:
    """Give the experiment as the device reads it: the host-only `meta` and `expect` left out."""
    return {k: v for k, v in experiment.items() if k not in ('meta', 'expect')}


def encode_batch(b: dict) -> bytes:
    """Give the bytes pushed to the device: one line of sorted JSON."""
    sent = {**b, 'experiments': [wire(e) for e in b['experiments']]}
    return (json.dumps(sent, sort_keys=True, separators=(',', ':')) + '\n').encode()


def decode_result(data: bytes) -> dict:
    result = json.loads(data)
    _require(result.get('protocol') == PROTOCOL, f'result protocol {result.get("protocol")!r}')
    _require(result.get('status') in STATUSES, f'result status {result.get("status")!r}')
    return result


# ok: ran; invalid: the device refused its description; refused: the raw path refused a draw or state; killed: the
# kernel killed the channel; crashed: the app ended while it ran (found by the journal at the next start); error: other
STATUSES = ('ok', 'invalid', 'refused', 'killed', 'crashed', 'error')
