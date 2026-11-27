// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap, HashSet};

use crate::packfile::{PlacedProgram, placed_from_compute};
use crate::records::{RawKernelRecord, RawProgram, fnv1a64};

pub const KERNEL_ENTRIES: usize = 64;
const CONSTEXPR_FLAG: u8 = 0x80;
const TEXTURE_ARRAY_FLAG: u8 = 0x80;
const BUFFER_KIND: u8 = 0;
const TEXTURE_KIND: u8 = 1;
const SAMPLER_KIND: u8 = 2;
const TEXTURE_PLANE_KIND: u8 = 6;
const TWO_PLANES: &str = "xr2p";
const FACTS_ONLY_TAG: &str = " | tg=0,0,0 | ";

#[derive(Clone, Debug)]
pub struct KernelEntry {
    pub off: u16,
    pub kind: u8,
    pub slot: u8,
    pub window: u32,
    pub residual: Option<u8>,
}

#[derive(Clone, Debug)]
pub struct KernelRecord {
    pub key: String,
    pub program: u32,
    pub prog_id: String,
    pub region: i8,
    pub qmd: [u32; 64],
    pub set_bytes: u16,
    pub read_only_entries: u64,
    pub push: BTreeMap<u8, u32>,
    pub entries: Vec<KernelEntry>,
    pub root_table_word: u8,
    pub status_word: u8,
    pub maps_stated: u8,
    pub root_slots: u32,
    pub heap: Vec<(u32, u32)>,
    pub heap_bytes: u32,
    pub spill_entry: u8,
    pub spill: Vec<u32>,
    pub stage_in_region: u8,
    pub stage_in_strides: [u8; 8],
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ComputeFacts {
    pub hash: u64,
    pub max_threads: u32,
    pub execution_width: u32,
    pub static_memory: u32,
}

fn texel_bytes(comp: u32) -> Option<u8> {
    match comp {
        0x01 => Some(16),
        0x02 => Some(12),
        0x03..=0x05 => Some(8),
        0x07..=0x09 | 0x0C..=0x0F | 0x20 | 0x21 => Some(4),
        0x12..=0x16 | 0x18 | 0x1B => Some(2),
        0x1D | 0x1E => Some(1),
        _ => None,
    }
}

fn format_bytes(fmt: i32) -> Option<u8> {
    match fmt {
        10..=14 => Some(1),
        20 | 22..=25 | 30..=34 | 40..=43 => Some(2),
        53..=55 | 60 | 62..=65 | 70..=74 | 80 | 81 | 90..=94 | 554 | 555 => Some(4),
        103..=105 | 110 | 112..=115 | 552 | 553 => Some(8),
        123..=125 => Some(16),
        _ => None,
    }
}

pub fn views_other_texel_size(key: &str, entries: &[KernelEntry]) -> Vec<u8> {
    let mut bound: HashMap<u8, i32> = HashMap::new();
    let mut planes: HashSet<u8> = HashSet::new();
    if let Some(tex_val) = find_tex_field(key) {
        for item in tex_val.split(',').filter(|s| !s.is_empty()) {
            let parts: Vec<&str> = item.split(':').collect();
            if parts.len() >= 2
                && let (Ok(slot), Ok(fmt)) = (parts[0].parse::<u8>(), parts[1].parse::<i32>())
            {
                bound.insert(slot, fmt);
                if parts.last().copied() == Some(TWO_PLANES) {
                    planes.insert(slot);
                }
            }
        }
    }
    let mut other = Vec::new();
    for e in entries {
        if (e.kind != TEXTURE_KIND && e.kind != TEXTURE_PLANE_KIND) || e.window == 0 {
            continue;
        }
        let texel = texel_bytes(e.window & 0x7F);
        let slot = e.slot & !TEXTURE_ARRAY_FLAG;
        let fmt = if planes.contains(&slot) {
            Some(if e.kind == TEXTURE_KIND { 4 } else { 1 })
        } else if e.kind == TEXTURE_PLANE_KIND {
            other.push(slot);
            continue;
        } else {
            format_bytes(*bound.get(&slot).unwrap_or(&-1))
        };
        if let (Some(t), Some(f)) = (texel, fmt)
            && t != f
        {
            other.push(slot);
        }
    }
    other
}

fn find_tex_field(key: &str) -> Option<&str> {
    let bytes = key.as_bytes();
    let mut i = 0;
    while i + 4 <= bytes.len() {
        let word_boundary = i == 0 || !(bytes[i - 1].is_ascii_alphanumeric() || bytes[i - 1] == b'_');
        if word_boundary && &bytes[i..i + 4] == b"tex=" {
            let start = i + 4;
            let mut end = start;
            while end < bytes.len() && bytes[end] != b' ' && bytes[end] != b'|' {
                end += 1;
            }
            return Some(&key[start..end]);
        }
        i += 1;
    }
    None
}

fn decode_entry(
    raw: &[u8],
    headers: &[[u32; 8]],
    constexpr_tsc: &mut Vec<[u32; 8]>,
    tsc_index: &mut HashMap<[u32; 8], usize>,
) -> KernelEntry {
    let off = u16::from_le_bytes([raw[0], raw[1]]);
    let kind = raw[2];
    let slot = raw[3];
    let window = u32::from_le_bytes(raw[4..8].try_into().expect("4 bytes"));
    if kind == SAMPLER_KIND {
        let header = headers[usize::from(slot & (CONSTEXPR_FLAG - 1))];
        let idx = *tsc_index.entry(header).or_insert_with(|| {
            let next = constexpr_tsc.len();
            constexpr_tsc.push(header);
            next
        });
        return KernelEntry {
            off,
            kind,
            slot: CONSTEXPR_FLAG | (idx as u8),
            window,
            residual: None,
        };
    }
    if kind == BUFFER_KIND {
        let residual_raw = (window >> 24) as i32 - 1;
        return KernelEntry {
            off,
            kind,
            slot,
            window: window & 0x00FF_FFFF,
            residual: (residual_raw >= 0).then_some(residual_raw as u8),
        };
    }
    KernelEntry {
        off,
        kind,
        slot,
        window,
        residual: None,
    }
}

fn decode_kernel(
    rec: &RawKernelRecord,
    constexpr_tsc: &mut Vec<[u32; 8]>,
    tsc_index: &mut HashMap<[u32; 8], usize>,
) -> KernelRecord {
    let k = &rec.kernel;
    let count = usize::from(k[16]);
    let region = k[17] as i8;
    let mut qmd = [0u32; 64];
    for (i, slot) in qmd.iter_mut().enumerate() {
        let at = 18 + 4 * i;
        *slot = u32::from_le_bytes(k[at..at + 4].try_into().expect("4 bytes"));
    }
    let set_bytes = u16::from_le_bytes([k[274], k[275]]);
    let read_only_entries = u64::from_le_bytes(k[276..284].try_into().expect("8 bytes"));
    let push_mask = u32::from_le_bytes(k[284..288].try_into().expect("4 bytes"));
    let mut push = BTreeMap::new();
    for w in 0..32u8 {
        if ((push_mask >> w) & 1) != 0 {
            let at = 288 + 4 * usize::from(w);
            let val = u32::from_le_bytes(k[at..at + 4].try_into().expect("4 bytes"));
            push.insert(w, val);
        }
    }
    let mut entries = Vec::with_capacity(count);
    for i in 0..count {
        let at = 416 + 8 * i;
        entries.push(decode_entry(&k[at..at + 8], &rec.samplers, constexpr_tsc, tsc_index));
    }
    let v20 = 928;
    let root_table_word = k[v20];
    let status_word = k[v20 + 1];
    let heap_arrays = usize::from(k[v20 + 2]);
    let maps_stated = k[v20 + 3];
    let root_slots = u32::from_le_bytes(k[v20 + 4..v20 + 8].try_into().expect("4 bytes"));
    let heap_bytes = u32::from_le_bytes(k[v20 + 8..v20 + 12].try_into().expect("4 bytes"));
    let mut heap = Vec::with_capacity(heap_arrays);
    for i in 0..heap_arrays {
        let at = v20 + 12 + 8 * i;
        let cls = u32::from_le_bytes(k[at..at + 4].try_into().expect("4 bytes"));
        let off = u32::from_le_bytes(k[at + 4..at + 8].try_into().expect("4 bytes"));
        heap.push((cls, off));
    }
    let spill_entry = k[v20 + 76];
    let spill_words = usize::from(k[v20 + 77]);
    let spill = rec.spill[..spill_words.min(rec.spill.len())].to_vec();
    let v21 = 1012;
    let stage_in_region = k[v21];
    let stage_in_strides: [u8; 8] = k[v21 + 1..v21 + 9].try_into().expect("8 bytes");
    KernelRecord {
        key: rec.key.clone(),
        program: 0,
        prog_id: rec.program_id.clone(),
        region,
        qmd,
        set_bytes,
        read_only_entries,
        push,
        entries,
        root_table_word,
        status_word,
        maps_stated,
        root_slots,
        heap,
        heap_bytes,
        spill_entry,
        spill,
        stage_in_region,
        stage_in_strides,
    }
}

pub fn from_records(
    records: &[RawKernelRecord],
    constexpr_tsc: &mut Vec<[u32; 8]>,
    tsc_index: &mut HashMap<[u32; 8], usize>,
) -> (Vec<KernelRecord>, Vec<(String, String)>) {
    let mut kernels = Vec::new();
    let mut skipped = Vec::new();
    let mut seen: HashSet<&str> = HashSet::new();
    for rec in records {
        if !seen.insert(rec.key.as_str()) {
            continue;
        }
        if rec.outcome != 0 {
            skipped.push((rec.key.clone(), rec.why.clone()));
            continue;
        }
        if rec.key.contains(FACTS_ONLY_TAG) {
            continue;
        }
        let made = decode_kernel(rec, constexpr_tsc, tsc_index);
        let bad_slots = views_other_texel_size(&rec.key, &made.entries);
        if !bad_slots.is_empty() {
            skipped.push((
                rec.key.clone(),
                format!("texture slots {bad_slots:?} viewed in texels of another size"),
            ));
            continue;
        }
        kernels.push(made);
    }
    (kernels, skipped)
}

pub fn state_facts(records: &[RawKernelRecord]) -> crate::Result<Vec<ComputeFacts>> {
    let mut by_hash: HashMap<u64, ComputeFacts> = HashMap::new();
    let mut order: Vec<u64> = Vec::new();
    for rec in records {
        if rec.outcome != 0 {
            continue;
        }
        let Some((max_threads, execution_width, static_memory)) = rec.facts else {
            continue;
        };
        let state = rec.key.split_once(" | tg=").map_or(rec.key.as_str(), |(a, _)| a);
        let hash = fnv1a64(state.as_bytes());
        let made = ComputeFacts {
            hash,
            max_threads,
            execution_width,
            static_memory,
        };
        if let Some(prev) = by_hash.get(&hash) {
            if prev != &made {
                return Err(crate::LinkError::Invalid(format!(
                    "compute state {hash:x} answers otherwise at another shape: {}",
                    rec.key
                )));
            }
        } else {
            order.push(hash);
            by_hash.insert(hash, made);
        }
    }
    Ok(order
        .into_iter()
        .map(|h| by_hash.remove(&h).expect("present"))
        .collect())
}

pub fn place_compute_program(
    pid: &str,
    programs: &HashMap<String, RawProgram>,
    heap: &mut Vec<u8>,
    prog_index: &mut HashMap<String, usize>,
    prog_recs: &mut Vec<PlacedProgram>,
) -> usize {
    if let Some(&existing) = prog_index.get(pid) {
        return existing;
    }
    let pr = &programs[pid];
    let pad = (0x100 - (heap.len() % 0x100)) % 0x100;
    heap.resize(heap.len() + pad, 0);
    let off = heap.len();
    let mut slot = pr.image[80..].to_vec();
    let slot_pad = (0x100 - (slot.len() % 0x100)) % 0x100;
    slot.resize(slot.len() + slot_pad, 0);
    let data_offset = if pr.data.is_empty() { 0 } else { slot.len() };
    slot.extend_from_slice(&pr.data);
    let slot_len = slot.len();
    heap.extend_from_slice(&slot);
    let idx = prog_recs.len();
    prog_index.insert(pid.to_owned(), idx);
    prog_recs.push(placed_from_compute(pid, pr, off, slot_len, data_offset));
    idx
}

pub fn lay_out(
    kernels: &mut [KernelRecord],
    programs: &HashMap<String, RawProgram>,
    heap: &mut Vec<u8>,
    prog_index: &mut HashMap<String, usize>,
    prog_recs: &mut Vec<PlacedProgram>,
) {
    for k in kernels.iter_mut() {
        let idx = place_compute_program(&k.prog_id, programs, heap, prog_index, prog_recs);
        k.program = idx as u32;
    }
}
