// SPDX-License-Identifier: GPL-3.0-or-later

use crate::kernels::{KERNEL_ENTRIES, KernelEntry, KernelRecord};
use crate::packfile::{
    ENTRY_ARRAY_VIEW, ENTRY_COMPARES, FAR_AT_LIMIT, HEAP_ARRAYS, PACK_ENTRIES, PipelineEntry, PipelineRecord,
    SET_ENTRIES_MAX, V20_FAR_AT, V22_SPILL_HIGH, VERTEX_STREAMS,
};
use crate::records::fnv1a64;
use crate::{LinkError, Result};

pub fn entry_bytes(entries: &[PipelineEntry]) -> Result<Vec<u8>> {
    let count = entries.len();
    if count > SET_ENTRIES_MAX {
        return Err(LinkError::Invalid(format!(
            "{count} set-0 entries: a record holds {SET_ENTRIES_MAX}"
        )));
    }
    let slots = count.max(PACK_ENTRIES);
    let mut out = vec![0u8; 8 * slots];
    for (i, e) in entries.iter().enumerate() {
        let at = i * 8;
        match *e {
            PipelineEntry::ColorTable { off } => {
                out[at..at + 2].copy_from_slice(&off.to_le_bytes());
                out[at + 2] = 5;
                out[at + 3] = 2;
            }
            PipelineEntry::Table { off, kind, stage } => {
                out[at..at + 2].copy_from_slice(&off.to_le_bytes());
                out[at + 2] = kind;
                out[at + 3] = stage;
            }
            PipelineEntry::Ubo {
                off,
                stage,
                slot,
                ssbo,
                residual,
                window,
            } => {
                out[at..at + 2].copy_from_slice(&off.to_le_bytes());
                out[at + 2] = 1;
                out[at + 3] = stage;
                out[at + 4] = slot;
                let word = residual.map_or(0u8, |r| (((r - 0x700) / 4 + 1) as u8) << 2);
                out[at + 5] = word | u8::from(ssbo);
                out[at + 6..at + 8].copy_from_slice(&((window / 16) as u16).to_le_bytes());
            }
            PipelineEntry::Handle {
                off,
                stage,
                tex,
                smp,
                compare,
                array_view,
            } => {
                out[at..at + 2].copy_from_slice(&off.to_le_bytes());
                out[at + 2] = tex.map_or(0, |(k, _)| k);
                out[at + 3] = stage;
                out[at + 4] = tex.map_or(0, |(_, s)| s);
                out[at + 5] = smp.map_or(0, |(k, _)| k);
                out[at + 6] = smp.map_or(0, |(_, s)| s);
                out[at + 7] =
                    (if compare { ENTRY_COMPARES } else { 0 }) | (if array_view { ENTRY_ARRAY_VIEW } else { 0 });
            }
        }
    }
    Ok(out)
}

pub fn far_words(rec: &PipelineRecord, encoded_entries: &[u8]) -> Vec<u32> {
    let count = rec.entries.len();
    if count <= PACK_ENTRIES {
        return Vec::new();
    }
    let far = &encoded_entries[8 * PACK_ENTRIES..];
    let masks = (count - PACK_ENTRIES).div_ceil(32);
    let mut words: Vec<u32> = far.as_chunks::<4>().0.iter().map(|c| u32::from_le_bytes(*c)).collect();
    for i in 0..masks {
        words.push(*rec.bound_words.get(1 + i).unwrap_or(&0));
    }
    for i in 0..masks {
        words.push(*rec.memory_words.get(1 + i).unwrap_or(&0));
    }
    words
}

pub fn far_at_bytes(at: usize) -> Result<[u8; 3]> {
    if at >= FAR_AT_LIMIT {
        return Err(LinkError::Invalid(format!(
            "a far block at blob word {at}: past the 24 bits that name it"
        )));
    }
    let b = (at as u32).to_le_bytes();
    Ok([b[0], b[1], b[2]])
}

#[allow(clippy::too_many_arguments)]
pub fn pipeline_bytes(
    rec: &PipelineRecord,
    key_offset: u32,
    blob_off: u32,
    variants_off: u32,
    name_offset: u32,
    keyhash: u64,
    vname: &str,
    entries_head: &[u8],
    spill_at: u32,
    restore_at: u32,
    far_at: usize,
    stride_twin: u64,
) -> Result<Vec<u8>> {
    let mut flags: u32 = u32::from(rec.fetch) | if rec.form == "barrier" { 2 } else { 0 };
    if rec.push_metadata {
        flags |= 4;
    }
    if rec.storage_mask != 0 {
        flags |= 8;
    }
    if !vname.is_empty() {
        flags |= 16;
    }
    if rec.form == "free" {
        flags |= 32;
    }
    if rec.turn_refused {
        flags |= 64;
    }
    if rec.raster_ordered {
        flags |= 256;
    }

    let entry_count = rec.entries.len();
    let mut out = Vec::with_capacity(736);
    out.extend_from_slice(&keyhash.to_le_bytes());
    out.extend_from_slice(&key_offset.to_le_bytes());
    out.extend_from_slice(&rec.vs.to_le_bytes());
    out.extend_from_slice(&rec.fs.to_le_bytes());
    out.extend_from_slice(&blob_off.to_le_bytes());
    out.extend_from_slice(&((rec.blob.len() * 2) as u32).to_le_bytes());
    out.push((entry_count & 0xFF) as u8);
    out.push(rec.push.len() as u8);
    out.push(rec.vb_slot);
    out.push((entry_count >> 8) as u8);
    out.extend_from_slice(&rec.vb_stride.to_le_bytes());
    out.extend_from_slice(&rec.set_size.to_le_bytes());
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(entries_head);

    let mut push_mask = 0u32;
    let mut push_words = [0u32; 32];
    for &(off, val) in &rec.push {
        let idx = ((off - 0x700) >> 2) as usize;
        push_mask |= 1u32 << idx;
        push_words[idx] = val;
    }
    out.extend_from_slice(&push_mask.to_le_bytes());
    for w in push_words {
        out.extend_from_slice(&w.to_le_bytes());
    }

    out.extend_from_slice(&rec.half_blend_color_word.to_le_bytes());
    out.extend_from_slice(&rec.iterated.to_le_bytes());

    out.push(rec.storage_mask);
    out.push(rec.storage_write_mask);
    out.push(rec.storage_samples);
    out.push(rec.storage_tests);
    for w in rec.storage_tic {
        out.extend_from_slice(&w.to_le_bytes());
    }

    out.push(rec.depth_test_word);
    out.push(rec.stencil_test_word);
    out.push(rec.test_flags);
    out.push(rec.shadow_stencil_word);
    out.extend_from_slice(&rec.depth_tic.to_le_bytes());
    out.extend_from_slice(&rec.stencil_tic.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());

    let c0 = rec.variants.iter().filter(|v| v.stage == 0).count() as u8;
    let c1 = rec.variants.iter().filter(|v| v.stage == 1).count() as u8;
    out.extend_from_slice(&(if c0 != 0 || c1 != 0 { variants_off } else { 0 }).to_le_bytes());
    out.push(c0);
    out.push(c1);
    out.push(rec.storage_blend_mask);
    out.push(rec.storage_blend_model);
    out.extend_from_slice(&rec.bound_words.first().copied().unwrap_or(0).to_le_bytes());
    out.extend_from_slice(&rec.uniform);
    out.extend_from_slice(&rec.fetch_rung);
    out.extend_from_slice(&name_offset.to_le_bytes());

    out.extend_from_slice(&rec.memory_words.first().copied().unwrap_or(0xFFFF_FFFF).to_le_bytes());

    if rec.heap.len() > HEAP_ARRAYS {
        return Err(LinkError::Invalid(format!(
            "{} set-1 heap arrays: a pipeline holds {HEAP_ARRAYS}",
            rec.heap.len()
        )));
    }
    let v20_start = out.len();
    out.extend_from_slice(&rec.root_table_word);
    out.extend_from_slice(&rec.status_word);
    out.extend_from_slice(&rec.root_slots[0].to_le_bytes());
    out.extend_from_slice(&rec.root_slots[1].to_le_bytes());
    out.push(rec.heap.len() as u8);
    out.extend_from_slice(&[0u8; 3]);
    out.extend_from_slice(&rec.heap_bytes.to_le_bytes());
    for i in 0..HEAP_ARRAYS {
        let (cls, off) = rec.heap.get(i).copied().unwrap_or((0, 0));
        out.extend_from_slice(&cls.to_le_bytes());
        out.extend_from_slice(&off.to_le_bytes());
    }
    out.push((rec.spill_entry[0] & 0xFF) as u8);
    out.push((rec.spill_entry[1] & 0xFF) as u8);
    out.push(rec.spill[0].len() as u8);
    out.push(rec.spill[1].len() as u8);
    let has_spill = !rec.spill[0].is_empty() || !rec.spill[1].is_empty();
    out.extend_from_slice(&(if has_spill { spill_at } else { 0 }).to_le_bytes());
    if entry_count > PACK_ENTRIES {
        let f = far_at_bytes(far_at)?;
        out[v20_start + V20_FAR_AT..v20_start + V20_FAR_AT + 3].copy_from_slice(&f);
    }

    out.extend_from_slice(&rec.runtime_head);
    out.push(rec.array_length_word);
    out.extend_from_slice(&rec.pulled_stride);
    let mut vs_buf = [0u8; VERTEX_STREAMS];
    let vs_n = rec.vertex_streams.len().min(VERTEX_STREAMS);
    vs_buf[..vs_n].copy_from_slice(&rec.vertex_streams[..vs_n]);
    out.extend_from_slice(&vs_buf);
    out.push(rec.viewport_count_word);

    let v22_start = out.len();
    for &wf in &rec.written_format {
        out.extend_from_slice(&wf.to_le_bytes());
    }
    out.extend_from_slice(&rec.xr_slots.to_le_bytes());
    out.extend_from_slice(&[0u8; 2]);
    if rec.spill_entry[0] > 0xFF || rec.spill_entry[1] > 0xFF {
        let high = (rec.spill_entry[0] >> 8) | ((rec.spill_entry[1] >> 8) << 8);
        out[v22_start + V22_SPILL_HIGH..v22_start + V22_SPILL_HIGH + 2].copy_from_slice(&high.to_le_bytes());
    }

    out.extend_from_slice(&rec.tess[0].to_le_bytes());
    out.extend_from_slice(&rec.tess[1].to_le_bytes());
    out.extend_from_slice(&stride_twin.to_le_bytes());
    out.push(rec.amplification_layer_word);
    out.push(rec.amplification_viewport_word);
    let restore_dwords = (2 * rec.restore.len()) as u16;
    out.extend_from_slice(&restore_dwords.to_le_bytes());
    out.extend_from_slice(&(if rec.restore.is_empty() { 0 } else { restore_at }).to_le_bytes());

    out.extend_from_slice(&rec.guest_base_word);
    out.extend_from_slice(&rec.guest_base_slot);
    debug_assert_eq!(out.len(), 736);
    Ok(out)
}

fn kernel_window(e: &KernelEntry) -> u32 {
    match e.residual {
        Some(r) => e.window | ((u32::from(r) + 1) << 24),
        None => e.window,
    }
}

pub fn kernel_bytes(k: &KernelRecord, key_offset: u32, spill_at: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(1024);
    out.extend_from_slice(&fnv1a64(k.key.as_bytes()).to_le_bytes());
    out.extend_from_slice(&key_offset.to_le_bytes());
    out.extend_from_slice(&k.program.to_le_bytes());
    out.push(k.entries.len() as u8);
    out.push(k.region as u8);
    for w in k.qmd {
        out.extend_from_slice(&w.to_le_bytes());
    }
    let mut mask = 0u32;
    let mut words = [0u32; 32];
    for (&w, &v) in &k.push {
        mask |= 1u32 << w;
        words[usize::from(w)] = v;
    }
    out.extend_from_slice(&k.set_bytes.to_le_bytes());
    out.extend_from_slice(&k.read_only_entries.to_le_bytes());
    out.extend_from_slice(&mask.to_le_bytes());
    for w in words {
        out.extend_from_slice(&w.to_le_bytes());
    }
    for i in 0..KERNEL_ENTRIES {
        if let Some(e) = k.entries.get(i) {
            out.extend_from_slice(&e.off.to_le_bytes());
            out.push(e.kind);
            out.push(e.slot);
            out.extend_from_slice(&kernel_window(e).to_le_bytes());
        } else {
            out.extend_from_slice(&[0u8; 8]);
        }
    }
    out.push(k.root_table_word);
    out.push(k.status_word);
    out.push(k.heap.len() as u8);
    out.push(k.maps_stated);
    out.extend_from_slice(&k.root_slots.to_le_bytes());
    out.extend_from_slice(&k.heap_bytes.to_le_bytes());
    for i in 0..HEAP_ARRAYS {
        let (cls, off) = k.heap.get(i).copied().unwrap_or((0, 0));
        out.extend_from_slice(&cls.to_le_bytes());
        out.extend_from_slice(&off.to_le_bytes());
    }
    out.push(k.spill_entry);
    out.push(k.spill.len() as u8);
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&(if k.spill.is_empty() { 0 } else { spill_at }).to_le_bytes());
    out.push(k.stage_in_region);
    out.extend_from_slice(&k.stage_in_strides);
    out.extend_from_slice(&[0u8; 3]);
    debug_assert_eq!(out.len(), 1024);
    out
}
