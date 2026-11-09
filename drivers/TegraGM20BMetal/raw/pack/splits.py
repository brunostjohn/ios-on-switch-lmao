#!/usr/bin/env python3
"""Fragment programs split for the raw path (Mesa nvk_switch_split.h): their place in the pack.

A split moves what a fragment program computes from per-draw values alone into a compute prologue, which writes those
values (the frontier) for the per-pixel program to read from a constant buffer of its own. The direct compile makes a
state's split with its record (directpack.mm); directlink.py packs the split of each packed state's fragment program
(runtime/raw_pack.h PackSplit).
"""

import kernels
import packfile

PROGRAM_CBUFS = 11
QMD_CBUFS = 8
FRONTIER = 6  # runtime/raw_pack.h CbufFrontier


def of_record(record: bytes, fs: str, prologue: str) -> dict:
    """Give a split from its PackSplit as the direct compile made it (no program indices yet) and its programs' ids."""
    fields = packfile.PACK_SPLIT.unpack(record)
    frontier_bytes, frontier_cbuf, ubos, shader_data_cbuf, prologue_cbufs, ubo = fields[4:10]
    return {
        'fs': fs,
        'prologue': prologue,
        'frontier_bytes': frontier_bytes,
        'frontier_cbuf': frontier_cbuf,
        'prologue_cbufs': prologue_cbufs,
        'shader_data_cbuf': shader_data_cbuf,
        'ubo_cbufs': list(ubo[:ubos]),
        'threshold': fields[0],
        'qmd': list(fields[10:]),
    }


def instruction_slots(program: dict) -> int:
    """Give a program's instruction slots: its code past the header, every fourth 8-byte word a scheduling word."""
    return (len(program['image']) - 80) // 32 * 3


def per_pixel(s: dict, whole: str, programs: dict[str, dict]) -> str | None:
    """Give the key of the split's per-pixel program bound as `whole` binds its constant buffers, or None.

    The pack keeps one program per id (its bytes), and the per-pixel parts of two wholes that bind a uniform buffer at
    different set-0 offsets (raw_pack.h cbufUboOffset: the offset is in the type) can compile to the same bytes.
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
        before += instruction_slots(programs[whole])
        after += instruction_slots(programs[fs])
    out.sort(key=lambda s: s['whole'])
    report.append(
        f'{len(out)} fragment programs split: {before} -> {after} instruction slots'
        + (f' ({100 * (after - before) / before:+.1f}%)' if before else '')
    )
    return out
