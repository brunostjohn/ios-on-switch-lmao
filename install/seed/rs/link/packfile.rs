// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap};

use crate::headers::own_states;
use crate::iterated_ucode::IteratedProgram;
use crate::kernels::KernelRecord;
use crate::records::{RawProgram, fnv1a64};
use crate::splits::SplitRecord;

pub const MAGIC: &[u8; 8] = b"GM20BPK1";
pub const VERSION: u32 = 37;
pub const SECTIONS: usize = 17;
pub const PACK_ENTRIES: usize = 32;
pub const SET_ENTRIES_MAX: usize = 370;
pub const FAR_AT_LIMIT: usize = 1 << 24;
pub const PROGRAM_CBUFS: usize = 18;
pub const PROGRAM_BYTES: usize = 48;
pub const PACK_SPLIT_BYTES: usize = 322;
pub const HEAP_ARRAYS: usize = 8;
pub const VERTEX_STREAMS: usize = 7;
pub const WRITTEN_SLOTS: usize = 16;
pub const GUEST_BASES: usize = 4;
pub const NO_PROGRAM: u32 = 0xFFFF_FFFF;
pub const TIER_ALIGN: usize = 128 << 10;
pub const ENTRY_COMPARES: u8 = 1;
pub const ENTRY_ARRAY_VIEW: u8 = 2;
pub const STREAM_BIND_STRIDE: u8 = 0x80;
pub const V20_FAR_AT: usize = 13;
pub const V22_SPILL_HIGH: usize = 34;

#[derive(Clone, Debug)]
pub enum PipelineEntry {
    ColorTable {
        off: u16,
    },
    Table {
        off: u16,
        kind: u8,
        stage: u8,
    },
    Ubo {
        off: u16,
        stage: u8,
        slot: u8,
        ssbo: bool,
        residual: Option<u32>,
        window: u32,
    },
    Handle {
        off: u16,
        stage: u8,
        tex: Option<(u8, u8)>,
        smp: Option<(u8, u8)>,
        compare: bool,
        array_view: bool,
    },
}

#[derive(Clone, Debug)]
pub struct PipelineVariant {
    pub stage: u8,
    pub assume: u32,
    pub program: u32,
    pub memory: u32,
}

#[derive(Clone, Debug)]
pub struct PipelineRecord {
    pub index: usize,
    pub key: String,
    pub form: String,
    pub tier: u32,
    pub vs: u32,
    pub fs: u32,
    pub blob: Vec<(u32, u32)>,
    pub restore: Vec<(u32, u32)>,
    pub tess: [u32; 2],
    pub runtime_head: [u8; 7],
    pub pulled_stride: [u8; 16],
    pub stride_twin: u64,
    pub amplification_layer_word: u8,
    pub amplification_viewport_word: u8,
    pub guest_base_word: [u8; 4],
    pub guest_base_slot: [u8; 4],
    pub push: Vec<(u32, u32)>,
    pub push_metadata: bool,
    pub turn_refused: bool,
    pub raster_ordered: bool,
    pub half_blend_color_word: u16,
    pub iterated: u16,
    pub iterated_name: String,
    pub fs_image: Vec<u8>,
    pub xr_slots: u16,
    pub storage_mask: u8,
    pub storage_write_mask: u8,
    pub storage_samples: u8,
    pub storage_tests: u8,
    pub storage_blend_mask: u8,
    pub storage_blend_model: u8,
    pub depth_test_word: u8,
    pub stencil_test_word: u8,
    pub test_flags: u8,
    pub shadow_stencil_word: u8,
    pub depth_tic: u32,
    pub stencil_tic: u32,
    pub storage_tic: [u32; 8],
    pub set_size: u16,
    pub entries: Vec<PipelineEntry>,
    pub vb_slot: u8,
    pub vb_stride: u16,
    pub vertex_streams: Vec<u8>,
    pub array_length_word: u8,
    pub viewport_count_word: u8,
    pub fetch: bool,
    pub bound_words: Vec<u32>,
    pub memory_words: Vec<u32>,
    pub root_table_word: [u8; 2],
    pub status_word: [u8; 2],
    pub root_slots: [u32; 2],
    pub heap: Vec<(u32, u32)>,
    pub heap_bytes: u32,
    pub spill_entry: [u16; 2],
    pub spill: [Vec<u32>; 2],
    pub uniform: [u8; 32],
    pub fetch_rung: [u8; 8],
    pub written_format: [u16; 16],
    pub variants: Vec<PipelineVariant>,
}

#[derive(Clone, Debug)]
pub struct PlacedProgram {
    pub id: String,
    pub storage_id: String,
    pub name: String,
    pub heap_offset: usize,
    pub program: usize,
    pub bytes: usize,
    pub code: usize,
    pub cbufs: Vec<u8>,
    pub data_offset: usize,
    pub data_bytes: usize,
    pub gprs: u8,
    pub bound_textures: u8,
    pub far_ubos: Vec<u16>,
    pub facts: u32,
}

pub struct Pack {
    pub pipes: Vec<PipelineRecord>,
    pub programs: Vec<PlacedProgram>,
    pub constexpr_tsc: Vec<[u32; 8]>,
    pub baseline: BTreeMap<u32, u32>,
    pub init: Vec<(u16, u16, u32, u32)>,
    pub heap: Vec<u8>,
    pub kernels: Vec<KernelRecord>,
    pub splits: Vec<SplitRecord>,
    pub archives: Vec<u8>,
    pub iterated: Vec<IteratedProgram>,
    pub compute_facts: Vec<crate::kernels::ComputeFacts>,
    pub reflections: BTreeMap<(bool, u64), Vec<u8>>,
    pub fast_keys: HashMap<u64, u64>,
    pub tier0_programs: Option<usize>,
    pub tier0_heap: Option<usize>,
    pub segments: Vec<u8>,
    pub tier0_slm_per_warp: u32,
}

pub fn placed_from_compute(pid: &str, pr: &RawProgram, off: usize, bytes: usize, data_offset: usize) -> PlacedProgram {
    PlacedProgram {
        id: pid.to_owned(),
        storage_id: pid.to_owned(),
        name: pr.program_name(),
        heap_offset: off,
        program: off,
        bytes,
        code: pr.image.len().saturating_sub(80),
        cbufs: pr.cbufs.clone(),
        data_offset,
        data_bytes: pr.data.len(),
        gprs: 0,
        bound_textures: 0,
        far_ubos: Vec::new(),
        facts: 0,
    }
}

pub fn place_graphics(
    pid: &str,
    programs: &HashMap<String, RawProgram>,
    heap: &mut Vec<u8>,
    prog_index: &mut HashMap<String, usize>,
    storage_index: &mut HashMap<String, usize>,
    prog_recs: &mut Vec<PlacedProgram>,
) {
    if prog_index.contains_key(pid) {
        return;
    }
    let pr = &programs[pid];
    let storage_id = pr.storage_id.as_deref().unwrap_or(pid);
    if let Some(&shared_idx) = storage_index.get(storage_id) {
        let mut copy = prog_recs[shared_idx].clone();
        copy.id = pid.to_owned();
        prog_index.insert(pid.to_owned(), prog_recs.len());
        prog_recs.push(copy);
        return;
    }
    storage_index.insert(storage_id.to_owned(), prog_recs.len());
    let off = heap.len();
    let mut slot = vec![0u8; 0x30];
    slot.extend_from_slice(&pr.image);
    let pad = (0x100 - (slot.len() % 0x100)) % 0x100;
    slot.resize(slot.len() + pad, 0);
    let data_offset = if pr.data.is_empty() { 0 } else { slot.len() };
    slot.extend_from_slice(&pr.data);
    let slot_len = slot.len();
    heap.extend_from_slice(&slot);
    prog_index.insert(pid.to_owned(), prog_recs.len());
    prog_recs.push(PlacedProgram {
        id: pid.to_owned(),
        storage_id: storage_id.to_owned(),
        name: pr.program_name(),
        heap_offset: off,
        program: off + 0x30,
        bytes: slot_len,
        code: pr.image.len().saturating_sub(80),
        cbufs: pr.cbufs.clone(),
        data_offset,
        data_bytes: pr.data.len(),
        gprs: pr.gprs,
        bound_textures: pr.bound_textures,
        far_ubos: pr.far_ubos.clone(),
        facts: pr.facts,
    });
}

pub fn variant_name(key: &str) -> &str {
    let mut end = key.len();
    let mut pos = key[..end].rfind(" | ");
    if let Some(p) = pos {
        let seg = &key[p + 3..end];
        if let Some(rest) = seg.strip_prefix("wtex=")
            && !rest.is_empty()
            && rest.bytes().all(|b| b.is_ascii_digit() || b == b':' || b == b',')
        {
            end = p;
            pos = key[..end].rfind(" | ");
        }
    }
    if let Some(p) = pos {
        let seg = &key[p + 3..end];
        if let Some(rest) = seg.strip_prefix("xrs=")
            && !rest.is_empty()
            && rest.bytes().all(|b| b.is_ascii_digit())
        {
            end = p;
            pos = key[..end].rfind(" | ");
        }
    }
    if let Some(p) = pos {
        let seg = &key[p + 3..end];
        if let Some(rest) = seg.strip_prefix("tests=")
            && !rest.is_empty()
            && rest.bytes().all(|b| b.is_ascii_digit())
        {
            end = p;
            pos = key[..end].rfind(" | ");
        }
    }
    if let Some(p) = pos {
        let seg = &key[p + 3..end];
        if seg.starts_with("cubic ") && !seg.contains('|') {
            end = p;
        }
    }
    &key[end..]
}

pub fn key_hash_str(raw: &str, vname: &str) -> u64 {
    let key = if vname.is_empty() {
        raw
    } else {
        &raw[..raw.len() - vname.len()]
    };
    if let Some(bar1) = key.find(" | ")
        && key[bar1 + 3..].starts_with("v ")
        && let Some(bar2_rel) = key[bar1 + 3..].find(" | ")
    {
        let bar2 = bar1 + 3 + bar2_rel;
        if !key[bar2 + 3..].contains(" | ") {
            let pair = format!("{} | {}", key[bar1 + 5..bar2].trim_end(), &key[bar2 + 3..]);
            if let Some(&hit) = own_states().get(&pair) {
                return hit;
            }
        }
    }
    fnv1a64(key.as_bytes())
}
