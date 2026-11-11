"""The batch and result files the host and HW Test's bench loop exchange (benchloop/1).

A batch is one JSON object; each experiment in it is validated here before it is written, and again by the app
(hw_test_app/bench) before it runs. Byte strings travel as base64. Limits keep every command buffer far below the
kernel's stuck-channel rule (500 ms without a completed fence), which bounds a hung experiment instead of a CPU timeout.
"""

import base64
import json
import re
from dataclasses import dataclass

PROTOCOL = 'benchloop/1'
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
    (0x0F90, 0x0F94),  # SET_SINGLE_CT_WRITE_CONTROL
    (0x0FAC, 0x0FB0),  # SET_CT_MRT_ENABLE
    (0x0FDC, 0x0FE0),  # SET_BLEND_OPT_CONTROL
    (0x10CC, 0x10D0),  # SET_REDUCE_COLOR_THRESHOLDS_UNORM8
    (0x10E0, 0x10F4),  # SET_REDUCE_COLOR_THRESHOLDS_UNORM10..SRGB8
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
    if experiment['kind'] in ('noop', 'known', 'twod'):
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


def _twod(t: object) -> None:
    _require(isinstance(t, dict) and t.get('op') in ('fill', 'scale'), 'twod is an object whose op is fill or scale')
    color = t.get('color', [0, 0, 0, 0])
    _require(isinstance(color, list) and len(color) == 4, 'twod.color is four words')
    for c in color:
        _count(c, 0, 0xFFFFFFFF, 'twod.color')
    for field in ('fmt2d', 'colorFormat'):
        _count(t.get(field, 0), 0, 255, f'twod.{field}')
    for field in ('du', 'dv', 'sx', 'sy'):
        _count(t.get(field, 0), 0, (1 << 63) - 1, f'twod.{field}')


def validate_experiment(e: dict) -> None:
    _require(isinstance(e, dict), 'an experiment is an object')
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
        c = e.get('compute')
        _program(c, 'compute')
        for field, hi in (('threads', MAX_THREADS), ('groups', MAX_GROUPS)):
            v = c.get(field)
            _require(isinstance(v, list) and len(v) == 3, f'compute.{field} is [x, y, z]')
            for n in v:
                _count(n, 1, hi, f'compute.{field}')
        _require(c['threads'][0] * c['threads'][1] * c['threads'][2] <= MAX_THREADS, 'compute.threads > 1024')
    if e['kind'] == 'twod':
        _twod(e.get('twod'))
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
