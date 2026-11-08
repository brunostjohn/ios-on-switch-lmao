#!/usr/bin/env python3
"""Parse nvk_shader objects from m2v driver records (M2VPPLC = NVK VkPipelineCache data).

The object data is nvk_shader_serialize's output (Mesa src/nouveau/vulkan/nvk_shader.c, MIT): nak_shader_info (864 B),
nvk_cbuf_map (132 B), sample_shading_enable (1), min_sample_shading (f32), shader version byte, then blob_write_uint32
(4-aligned) code_size, data_size, code, data, asm string, nir string, and a fragment program's split for the raw path
(nvk_switch_split.c switch_nvk_split_serialize).
"""

import struct
import sys
from collections.abc import Iterator

NAK_INFO = 864
SPLIT_MAGIC = 0x5053564E
SPLIT_UBOS = 6
CBUF_TYPES = {0: 'invalid', 1: 'root', 2: 'shader_data', 3: 'desc_set', 4: 'dynamic_ubo', 5: 'ubo_desc'}


def records(b: bytes) -> Iterator[tuple[int, bytes, bytes]]:
    assert b[:8] == b'M2VPPLC\0', 'not a record'
    n = struct.unpack_from('<Q', b, 36)[0]
    data = b[44 : 44 + n]
    at = struct.unpack_from('<I', data, 0)[0]
    count = struct.unpack_from('<I', data, at)[0]
    at += 4
    for _ in range(count):
        at = (at + 3) // 4 * 4
        t, ks, ds = struct.unpack_from('<iII', data, at)
        at += 12
        key = data[at : at + ks]
        at += ks
        at = (at + 7) // 8 * 8
        yield t, key, data[at : at + ds]
        at += ds


def shader(d: bytes) -> dict | None:
    """Return a dict for an nvk_shader object, or None for other objects."""
    if len(d) < NAK_INFO + 150 or d[4] != 53:  # sm 53 (GM20B)
        return None
    stage, _sm, gprs = struct.unpack_from('<IBB', d, 0)
    at = NAK_INFO
    count = struct.unpack_from('<I', d, at)[0]
    cbufs = [struct.unpack_from('<BBBBI', d, at + 4 + 8 * i) for i in range(16)][:count]
    at += 132
    sse = d[at]
    mss = struct.unpack_from('<f', d, at + 1)[0]
    at += 6
    at = (at + 3) // 4 * 4
    code_size, data_size = struct.unpack_from('<II', d, at)
    at += 8
    code = d[at : at + code_size]
    at += code_size
    cdata = d[at : at + data_size]
    at += data_size
    hdr = d[NAK_INFO - 128 : NAK_INFO]
    info = d[:NAK_INFO]
    for _ in range(2):  # the asm and NIR strings
        at = d.index(b'\0', at) + 1
    return {
        'stage': stage,
        'gprs': gprs,
        'cbufs': [(CBUF_TYPES.get(t, t), s, di, off) for t, s, di, _, off in cbufs],
        'code': code,
        'data': cdata,
        'hdr': hdr,
        'info': info,
        'sample_shading': sse,
        'min_sample_shading': mss,
        'instrs': struct.unpack_from('<I', d, 12)[0],
        'cycles': struct.unpack_from('<Q', d, 16)[0],
        'slm': struct.unpack_from('<I', d, 40)[0],
        'crs': struct.unpack_from('<I', d, 44)[0],
        'fs': d[48:53],
        'vtg': d[188:193],
        'rest': len(d) - at,
        'split': split(d, at),
    }


def _u32(d: bytes, at: int) -> tuple[int, int]:
    at = (at + 3) // 4 * 4
    return struct.unpack_from('<I', d, at)[0], at + 4


def _program(d: bytes, at: int) -> tuple[dict, int]:
    info = d[at : at + NAK_INFO]
    at += NAK_INFO
    count, at = _u32(d, at)
    types = list(d[at : at + count])
    at += 16
    code_size, at = _u32(d, at)
    code = d[at : at + code_size]
    at += code_size
    data_size, at = _u32(d, at)
    data = d[at : at + data_size]
    at += data_size
    program = {
        'hdr': info[NAK_INFO - 128 :],
        'gprs': info[5],
        'instrs': struct.unpack_from('<I', info, 12)[0],
        'cycles': struct.unpack_from('<Q', info, 16)[0],
        'cbuf_codes': types,
        'code': code,
        'data': data,
    }
    return program, at


def split(d: bytes, at: int) -> dict | None:
    """Return a fragment program's split (its program, its prologue, their layout and QMD), or None."""
    if at + 4 > len(d):
        return None
    magic, at = _u32(d, at)
    if magic != SPLIT_MAGIC:
        return None
    layout = struct.unpack_from('<8I', d, at)
    at += 32
    ubos = list(d[at : at + layout[6]])
    at += SPLIT_UBOS
    qmd = d[at : at + 256]
    at += 256
    fs, at = _program(d, at)
    prologue, at = _program(d, at)
    return {
        'frontier_bytes': layout[0],
        'frontier_cbuf': layout[1],
        'prologue_cbufs': layout[2],
        'shader_data_cbuf': layout[3],
        'frontier_values': layout[4],
        'prologue_moved': layout[5],
        'ubo_cbufs': ubos,
        'threshold': layout[7],
        'qmd': qmd,
        'fs': fs,
        'prologue': prologue,
    }


if __name__ == '__main__':
    for f in sys.argv[1:]:
        with open(f, 'rb') as head:
            if head.read(8) != b'M2VPPLC\0':
                continue
        with open(f, 'rb') as whole:
            data = whole.read()
        for _t, _k, d in records(data):
            s = shader(d)
            if s:
                print(
                    f[-8:],
                    'stage',
                    s['stage'],
                    'gprs',
                    s['gprs'],
                    'code',
                    len(s['code']),
                    'data',
                    len(s['data']),
                    'cbufs',
                    s['cbufs'],
                    'slm',
                    s['slm'],
                    'crs',
                    s['crs'],
                    'tail',
                    s['rest'],
                    'code0',
                    s['code'][:8].hex(),
                )
