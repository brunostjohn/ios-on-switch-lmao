#!/usr/bin/env python3
"""Fragment programs split for the raw path (Mesa nvk_switch_split.h): their records and their place in the pack.

A split moves what a fragment program computes from per-draw values alone into a compute prologue, which writes those
values (the frontier) for the per-pixel program to read from a constant buffer of its own. mkprograms.py writes a
splits.bin record per split beside programs.bin; mkpack.py packs the split of each packed state's fragment program
(runtime/raw_pack.h PackSplit).
"""

import os
import struct

import kernels
import packfile

# 'NVSL', the whole, per-pixel and prologue programs' ids (mkpack.py load_programs), the layout (frontier bytes,
# frontier cbuf, prologue cbufs, prologue shader data cbuf, frontier values, NIR instructions moved, UBO count,
# threshold pixels), the UBOs' fragment cbufs, the prologue's QMD, then the instructions and static cycles of the whole
# program, the per-pixel one and the prologue
RECORD = struct.Struct('<4sQQQ8I8s256s6Q')
PROGRAM_CBUFS = 11
QMD_CBUFS = 8
FRONTIER = 6  # runtime/raw_pack.h CbufFrontier


def record(ids: tuple[int, int, int], split: dict, whole: dict) -> bytes:
    layout = (
        split['frontier_bytes'],
        split['frontier_cbuf'],
        split['prologue_cbufs'],
        split['shader_data_cbuf'],
        split['frontier_values'],
        split['prologue_moved'],
        len(split['ubo_cbufs']),
        split['threshold'],
    )
    fs, prologue = split['fs'], split['prologue']
    stats = (whole['instrs'], whole['cycles'], fs['instrs'], fs['cycles'], prologue['instrs'], prologue['cycles'])
    return RECORD.pack(b'NVSL', *ids, *layout, bytes(split['ubo_cbufs']), split['qmd'], *stats)


def load(dirs: list[str]) -> dict[str, dict]:
    """Give the buckets' splits by their whole program's id."""
    out = {}
    for d in dirs:
        path = os.path.join(d, 'splits.bin')
        if not os.path.exists(path):
            continue
        with open(path, 'rb') as f:
            b = f.read()
        for at in range(0, len(b) - RECORD.size + 1, RECORD.size):
            r = RECORD.unpack_from(b, at)
            whole, fs, prologue = (f'{x:x}' for x in r[1:4])
            layout = r[4:12]
            out[whole] = {
                'fs': fs,
                'prologue': prologue,
                'frontier_bytes': layout[0],
                'frontier_cbuf': layout[1],
                'prologue_cbufs': layout[2],
                'shader_data_cbuf': layout[3],
                'frontier_values': layout[4],
                'ubo_cbufs': list(r[12][: layout[6]]),
                'threshold': layout[7],
                'qmd': list(struct.unpack('<64I', r[13])),
                'stats': r[14:20],
            }
    return out


def per_pixel(s: dict, whole: str, programs: dict[str, dict]) -> str | None:
    """Give the key of the split's per-pixel program bound as `whole` binds its constant buffers, or None.

    programs.bin keeps one record per program's bytes, and the per-pixel parts of two wholes that bind a uniform buffer
    at different set-0 offsets (mkprograms.py cbuf_code: the offset is in the type) can compile to the same bytes.
    """
    types = programs[whole]['cbufs'] + [FRONTIER]
    if s['frontier_cbuf'] != len(types) - 1:
        return None
    if programs[s['fs']]['cbufs'] == types:
        return s['fs']
    key = f'{s["fs"]} {bytes(types).hex()}'
    programs.setdefault(key, dict(programs[s['fs']], cbufs=types, id=s['fs']))
    return key


def lay_out(
    used: list[str], found: dict[str, dict], programs: dict[str, dict], laid: tuple, report: list[str]
) -> list[dict]:
    """Give the splits of the fragment programs `used`, their programs appended to the heap image `laid` holds."""
    heap, prog_index, prog_recs = laid
    out, before, after = [], 0, 0
    for whole in used:
        s = found.get(whole)
        if not s or s['fs'] not in programs or s['prologue'] not in programs:
            continue
        fs = per_pixel(s, whole, programs)
        if not fs or len(programs[fs]['cbufs']) > PROGRAM_CBUFS or s['prologue_cbufs'] > QMD_CBUFS:
            continue
        packfile.place_graphics(fs, programs, heap, prog_index, prog_recs)
        kernel = {'prog': s['prologue']}
        kernels.lay_out([kernel], programs, heap, prog_index, prog_recs)
        out.append(dict(s, whole=prog_index[whole], fs=prog_index[fs], prologue=kernel['program']))
        before += s['stats'][0]
        after += s['stats'][2]
    out.sort(key=lambda s: s['whole'])
    report.append(
        f'{len(out)} fragment programs split: {before} -> {after} instructions'
        + (f' ({100 * (after - before) / before:+.1f}%)' if before else '')
    )
    return out
