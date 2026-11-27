// SPDX-License-Identifier: GPL-3.0-or-later

use crate::iterated_ucode::IteratedProgram;
use crate::packfile::{PipelineRecord, PipelineVariant, PlacedProgram};

pub(crate) const PACK_WINDOW_VA: u64 = (1u64 << 39) - (4u64 << 30);
pub(crate) const PREFETCH: usize = 0x1000;
pub(crate) const SEGMENT_BIND_WORDS: usize = 8;
pub(crate) const SEGMENT_MAX_WORDS: usize = 0xFFFF;

pub(crate) const FLAG_KEPT_ACROSS_PASSES: u16 = 1 << 0;
pub(crate) const FLAG_RESELECT_ROOT: u16 = 1 << 1;
pub(crate) const FLAG_BLEND_KILL: u16 = 1 << 2;

const CBUF_SHADER_DATA: u8 = 2;
const CBUF_UNIT: usize = 256;
const CBUF_MAX_BYTES: usize = 0x10000;
pub(crate) const SET_GROUPS: [u32; 4] = [0, 4, 1, 2];

pub(crate) const CT_WRITE: u32 = 0x1A00;
pub(crate) const PIXEL_SHADER_INTERLOCK_CONTROL: u32 = 0x1224;
pub(crate) const HYBRID_ANTI_ALIAS_CONTROL: u32 = 0x0754;
pub(crate) const ITERATED_BLEND_OPTIMIZATION: u32 = 0x02F4;
pub(crate) const NOOP_NEVER: u32 = 0;
pub(crate) const NOOP_SOURCE_RGBA_0000: u32 = 1;

pub(crate) const SET_PIPELINE_PROGRAM_VS: u32 = 0x2044;
pub(crate) const SET_PIPELINE_REGISTER_COUNT_VS: u32 = 0x204C;
pub(crate) const SET_PIPELINE_PROGRAM_FS: u32 = 0x2144;
pub(crate) const SET_PIPELINE_REGISTER_COUNT_FS: u32 = 0x214C;
pub(crate) const SET_PIPELINE_PROGRAM_TESS: u32 = 0x2084;
pub(crate) const SET_PIPELINE_REGISTER_COUNT_TESS: u32 = 0x208C;
pub(crate) const PIPELINE_STAGE_STRIDE: u32 = 0x40;
pub(crate) const SET_PATCH: u32 = 0x0DCC;
pub(crate) const SET_BINDLESS_TEXTURE: u32 = 0x2608;
const CB_SELECTOR_A: u32 = 0x2380;
const BIND_GROUP_CB: u32 = 0x2410;
pub(crate) const PIXEL_STAGE_SWITCH: u32 = 0x2140;
pub(crate) const PIXEL_STAGE_OFF: u32 = 0x50;
pub(crate) const RASTER_SWITCH: u32 = 0x037C;

const FETCH_BLEND: u8 = 2;
const FETCH_ITERATED_BLEND: u8 = 3;
const FETCH_RUNG_MASK: u8 = 0x0F;

const PASS_WRITE_RANGES: &[(u32, u32)] = &[
    (0x0DC0, 0x0DC4),
    (0x0DC4, 0x0DC8),
    (0x0DC8, 0x0DCC),
    (0x15BC, 0x15C0),
    (0x156C, 0x1570),
    (0x187C, 0x1880),
    (0x1918, 0x191C),
    (0x191C, 0x1920),
    (0x1920, 0x1924),
    (0x0DAC, 0x0DB0),
    (0x0DB0, 0x0DB4),
    (0x1438, 0x143C),
    (0x1434, 0x1438),
    (0x1118, 0x111C),
    (0x0A18, 0x0C18),
    (0x19E0, 0x1A00),
    (0x0D80, 0x0D90),
    (0x0D6C, 0x0D74),
    (0x10F8, 0x10FC),
    (0x1944, 0x1948),
    (0x0D90, 0x0D94),
    (0x0DA0, 0x0DA4),
    (0x1538, 0x153C),
    (0x179C, 0x17A0),
    (0x19CC, 0x19D0),
    (0x111C, 0x1120),
    (0x1208, 0x120C),
    (0x0370, 0x0374),
    (0x02E8, 0x02EC),
    (0x02F8, 0x0300),
    (0x07C0, 0x07F8),
    (0x0DBC, 0x0DC0),
    (0x0DD8, 0x0DDC),
    (0x1590, 0x1594),
    (0x15C8, 0x15CC),
    (0x15FC, 0x1604),
];

pub(crate) fn is_pass_write_method(m: u32) -> bool {
    PASS_WRITE_RANGES
        .iter()
        .any(|&(lo, hi)| m >= lo && m < hi && ((m - lo) & 3) == 0)
}

fn is_normalized_format(fmt: u32) -> bool {
    matches!(
        fmt,
        1 | 10
            | 11
            | 12
            | 20
            | 22
            | 30
            | 31
            | 32
            | 40
            | 41
            | 42
            | 43
            | 60
            | 62
            | 70
            | 71
            | 72
            | 80
            | 81
            | 90
            | 94
            | 110
            | 112
    )
}

pub(crate) fn hdr(op: u32, mthd: u32, count: u32) -> u32 {
    (op << 29) | (count << 16) | (mthd >> 2)
}

pub(crate) fn push_m(out: &mut Vec<u32>, mthd: u32, val: u32) {
    if val < 0x2000 {
        out.push((4 << 29) | (val << 16) | (mthd >> 2));
    } else {
        out.push((1 << 29) | (1 << 16) | (mthd >> 2));
        out.push(val);
    }
}

pub(crate) fn push_inc(out: &mut Vec<u32>, mthd: u32, vals: &[u32]) {
    out.push((1 << 29) | ((vals.len() as u32) << 16) | (mthd >> 2));
    out.extend_from_slice(vals);
}

pub(crate) fn push_0inc(out: &mut Vec<u32>, mthd: u32, vals: &[u32]) {
    out.push((3 << 29) | ((vals.len() as u32) << 16) | (mthd >> 2));
    out.extend_from_slice(vals);
}

pub(crate) fn last_written(words: &[u32], method: u32, absent: u32) -> u32 {
    let mut value = absent;
    let mut i = 0usize;
    while i < words.len() {
        let w = words[i];
        let op = w >> 29;
        let arg = ((w >> 16) & 0x1FFF) as usize;
        let mthd = (w & 0xFFF) << 2;
        let mine = ((w >> 13) & 7) == 0;
        if op == 4 {
            if mine && mthd == method {
                value = arg as u32;
            }
            i += 1;
            continue;
        }
        if matches!(op, 1 | 3 | 5) {
            for k in 0..arg {
                if i + 1 + k >= words.len() {
                    break;
                }
                let at = if op == 1 {
                    mthd + 4 * (k as u32)
                } else if op == 5 && k > 0 {
                    mthd + 4
                } else {
                    mthd
                };
                if mine && at == method {
                    value = words[i + 1 + k];
                }
            }
            i += 1 + arg;
        } else {
            i += 1;
        }
    }
    value
}

fn cbuf_bind_bytes(size: usize) -> u32 {
    ((size + CBUF_UNIT - 1) & !(CBUF_UNIT - 1)).min(CBUF_MAX_BYTES) as u32
}

pub(crate) fn legal_interlock(interlock: u32, hybrid: u32) -> u32 {
    if (hybrid & 0xF) > 1 && (interlock & 3) == 2 {
        (interlock & !3) | 1
    } else {
        interlock
    }
}

fn key_color_format(key_string: &str, target: usize) -> u32 {
    let tag = format!(" c{target}=");
    let Some(at) = key_string.find(&tag) else {
        return 0;
    };
    let rest = &key_string[at + tag.len()..];
    let end = rest.bytes().take_while(u8::is_ascii_digit).count();
    if end == 0 {
        0
    } else {
        rest[..end].parse::<u32>().unwrap_or(0)
    }
}

pub(crate) fn color_formats(key_string: &str) -> [u32; 8] {
    let mut formats = [0u32; 8];
    let mut start = 0usize;
    while let Some(rel) = key_string[start..].find(" c") {
        start += rel + 2;
        let Some(eq_rel) = key_string[start..].find('=') else {
            break;
        };
        let eq = start + eq_rel;
        if eq <= start {
            continue;
        }
        let idx_s = &key_string[start..eq];
        if !idx_s.bytes().all(|b| b.is_ascii_digit()) {
            continue;
        }
        let Ok(c) = idx_s.parse::<usize>() else {
            continue;
        };
        if c >= 8 {
            continue;
        }
        let tail_full = &key_string[eq + 1..];
        let tail = tail_full.split_once(' ').map_or(tail_full, |(t, _)| t);
        let parts: Vec<&str> = tail.split(':').collect();
        if parts.len() >= 4
            && parts[..4]
                .iter()
                .all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
        {
            let fmt = parts[0].parse::<u32>().unwrap_or(0);
            formats[c] = if fmt == 542 || (550..=555).contains(&fmt) {
                115
            } else {
                fmt
            };
        }
    }
    formats
}

fn noop_source_coeff(c: u32, normalized: bool) -> bool {
    if matches!(c, 0x4000 | 0x4001 | 0x4300..=0x4303) {
        return true;
    }
    normalized && (0x4304..=0x4308).contains(&c)
}

pub(crate) fn iterated_blend_noop(
    blob: &[(u32, u32)],
    writes: &[u32; 8],
    key_string: &str,
    storage_bits: u8,
    default_per_target: [u32; 6],
    default_seen: u8,
) -> u32 {
    if (writes[0] & 0x1111) == 0 || (storage_bits & 1) != 0 || key_string.is_empty() {
        return NOOP_NEVER;
    }
    for (j, &w) in writes.iter().enumerate().skip(1) {
        if (w & 0x1111) != 0 && key_color_format(key_string, j) != 0 {
            return NOOP_NEVER;
        }
    }
    let mut blends = false;
    let mut per_target = default_per_target;
    let mut seen = default_seen;
    for &(m, v) in blob {
        if m == 0x153C && (v & 0x11) != 0 {
            return NOOP_NEVER;
        }
        if m == 0x1360 {
            blends = (v & 1) != 0;
        } else if (0x1E04..=0x1E18).contains(&m) {
            let idx = ((m - 0x1E04) >> 2) as usize;
            per_target[idx] = v;
            seen |= 1u8 << idx;
        }
    }
    if seen != 0x3F || !blends {
        return NOOP_NEVER;
    }
    let normalized = is_normalized_format(key_color_format(key_string, 0));
    for half in [0usize, 3] {
        let op_ok = matches!(per_target[half], 0x8006 | 0x800B);
        let src_ok = noop_source_coeff(per_target[half + 1], normalized);
        let dst_ok = matches!(per_target[half + 2], 0x4001 | 0x4301 | 0x4303);
        if !op_ok || !src_ok || !dst_ok {
            return NOOP_NEVER;
        }
    }
    NOOP_SOURCE_RGBA_0000
}

pub(crate) fn bind_program_cbufs(out: &mut Vec<u32>, prog: &PlacedProgram, group: u32) -> bool {
    let mut reselect = false;
    let data_va = if prog.data_bytes != 0 {
        PACK_WINDOW_VA + (prog.heap_offset + prog.data_offset) as u64
    } else {
        0
    };
    let bind_size = cbuf_bind_bytes(prog.data_bytes);
    let hi = (data_va >> 32) as u32;
    let lo = data_va as u32;
    let bind_mthd = BIND_GROUP_CB + 0x20 * group;
    for (c, &cbuf) in prog.cbufs.iter().enumerate().skip(1) {
        if cbuf != CBUF_SHADER_DATA {
            continue;
        }
        push_inc(out, CB_SELECTOR_A, &[bind_size, hi, lo]);
        push_m(out, bind_mthd, 1 | ((c as u32) << 4));
        reselect = true;
    }
    reselect
}

pub(crate) fn fetch_free_iterated<'a>(
    rec: &PipelineRecord,
    iterated_list: &'a [IteratedProgram],
) -> Option<(usize, &'a IteratedProgram)> {
    if rec.form != "free" {
        return None;
    }
    let it_idx = usize::from(rec.iterated);
    let it = if it_idx >= 1 && it_idx <= iterated_list.len() {
        Some(&iterated_list[it_idx - 1])
    } else {
        None
    };
    for (c, &fr) in rec.fetch_rung.iter().enumerate() {
        let rung = fr & FETCH_RUNG_MASK;
        if rung != FETCH_BLEND && rung != FETCH_ITERATED_BLEND {
            continue;
        }
        if rung != FETCH_ITERATED_BLEND {
            return None;
        }
        let it = it?;
        let matrix = it.words.is_empty() && it.uniform_slot < 31;
        if !matrix && (it.words.is_empty() || it.words.len() > 128) {
            return None;
        }
        if matrix && !(10..=11).contains(&it.uniform_words) {
            return None;
        }
        return Some((c, it));
    }
    None
}

pub(crate) fn emit_iterated_blend(out: &mut Vec<u32>, it: &IteratedProgram, colour: usize, fmt: u32) {
    out.push(hdr(1, 0x1360 + 4 * (colour as u32), 1));
    out.push(1);
    if fmt == 70 || fmt == 80 {
        out.push(hdr(1, 0x13A8, 1));
        out.push(1u32 << (4 * colour));
    }
    push_m(out, 0x12E4, 0);
    push_inc(out, 0x133C, &[1, 0x8006, 0x4001, 0x4000, 0x8006, it.alpha_source]);
    push_m(out, 0x1358, it.alpha_dest);
    if !it.words.is_empty() {
        for j in 0..it.constants.len() {
            let tri = it.constants.get(&(j as u32)).copied().unwrap_or([0, 0, 0]);
            push_inc(
                out,
                0x1480 + 16 * (j as u32),
                &[u32::from(tri[0]), u32::from(tri[1]), u32::from(tri[2])],
            );
        }
        push_m(out, 0x02F4, 0);
        push_m(out, 0x0DF0, 0);
        push_0inc(out, 0x0DF4, &it.words);
        push_m(out, 0x0DD4, it.words.len() as u32);
        push_m(out, 0x0DD0, 1 | if (it.enable & 2) != 0 { 2 } else { 0 });
    }
}

pub(crate) fn choose_variant(variants: &[&PipelineVariant], facts: u32) -> usize {
    for (i, v) in variants.iter().enumerate() {
        if (v.assume & !facts) == 0 {
            return i;
        }
    }
    variants.len()
}
