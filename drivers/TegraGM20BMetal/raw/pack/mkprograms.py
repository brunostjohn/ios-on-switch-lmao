#!/usr/bin/env python3
"""programs.bin for nvdec from NVK driver records under m2v cache dirs, and splits.bin beside it.

Per nvk_shader: 'NVPG', code_size, cbuf count, data_size, the cbufs (cbuf_code), the 80-byte shader header (the hdr NVK
uploads before the code on pre-Turing), the code, then NAK's shader data (the constant buffer of type shader_data; NVK
uploads it after the code, 256-aligned). A fragment program's split (nvk_switch_split.h) adds its per-pixel program and
its prologue as programs of their own, and a splits.bin record (splits.py).
usage: mkprograms.py <out programs.bin> <cache dir>...
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvkshader
import packfile
import splits

TYPES = {'invalid': 0, 'root': 1, 'shader_data': 2, 'desc_set': 3, 'dynamic_ubo': 4, 'ubo_desc': 5}


def cbuf_code(cbuf: tuple) -> int:
    """Return a constant buffer's code as the pack names it.

    The nvk_cbuf_type, and a UBO descriptor's set-0 offset / 16 in bits 3-7; a UBO elsewhere is 7 (runtime/raw_pack.h
    PackCbufType, nvk_switch_capture.c switch_nvk_cbuf_code).
    """
    kind, desc_set, _dynamic, offset = cbuf
    t = TYPES.get(kind, kind) if isinstance(kind, str) else kind
    if t != TYPES['ubo_desc']:
        return t
    if desc_set != 0 or offset % 16 or offset >= 512:
        return 7
    return t | (offset // 16) << 3


def program_record(hdr: bytes, code: bytes, data: bytes, types: bytes) -> bytes:
    return b'NVPG' + struct.pack('<III', len(code), len(types), len(data)) + types + hdr[:80] + code + data


def main() -> None:
    out, split_out, seen = bytearray(), bytearray(), set()

    def add(hdr: bytes, code: bytes, data: bytes, types: bytes) -> int:
        key = hdr[:80] + code + data
        if key not in seen:
            seen.add(key)
            out.extend(program_record(hdr, code, data, types))
        return packfile.fnv(key)

    for d in sys.argv[2:]:
        for root, _, files in os.walk(d):
            for f in files:
                p = os.path.join(root, f)
                with open(p, 'rb') as record:
                    b = record.read()
                if b[:8] != b'M2VPPLC\0':
                    continue
                for _t, _k, data in nvkshader.records(b):
                    s = nvkshader.shader(data)
                    if not s:
                        continue
                    whole = add(s['hdr'], s['code'], s['data'], bytes(cbuf_code(c) for c in s['cbufs']))
                    split = s['split']
                    if split:
                        fs, prologue = split['fs'], split['prologue']
                        ids = (
                            whole,
                            add(fs['hdr'], fs['code'], fs['data'], bytes(fs['cbuf_codes'])),
                            add(bytes(80), prologue['code'], prologue['data'], bytes(prologue['cbuf_codes'])),
                        )
                        split_out += splits.record(ids, split, s)
    with open(sys.argv[1], 'wb') as programs:
        programs.write(out)
    with open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'splits.bin'), 'wb') as split_file:
        split_file.write(split_out)
    print(f'{sys.argv[1]}: {len(seen)} programs, {len(split_out) // splits.RECORD.size} splits')


if __name__ == '__main__':
    main()
