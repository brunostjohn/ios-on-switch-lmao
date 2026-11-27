// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;

use crate::packfile::{
    ENTRY_ARRAY_VIEW, ENTRY_COMPARES, NO_PROGRAM, PACK_ENTRIES, PipelineEntry, PipelineRecord, PipelineVariant,
    STREAM_BIND_STRIDE,
};
use crate::probelog::{PROGRAM_OFFSETS, ProbeView, is_foreign};
use crate::records::RawStateRecord;
use crate::{LinkError, Result};

const PIPELINE_BYTES: usize = 736;
const STREAM_FORMAT: u32 = 0x1C00;
const STREAM_STRIDE: u32 = 0xFFF;

const V20_AT: usize = 544;
const V21_AT: usize = 636;
const V22_AT: usize = 668;
const V24_AT: usize = 704;
const V36_AT: usize = 728;

fn layout_strides(key: &str) -> HashMap<u8, u32> {
    let mut out = HashMap::new();
    let head = key.split_once(" | ").map_or(key, |(h, _)| h);
    for field in head.split_ascii_whitespace() {
        if let Some(rest) = field.strip_prefix('a')
            && let Some((idx_s, val_s)) = rest.split_once('=')
            && !idx_s.is_empty()
            && idx_s.bytes().all(|b| b.is_ascii_digit())
        {
            let parts: Vec<&str> = val_s.split(':').collect();
            if parts.len() == 6
                && parts
                    .iter()
                    .all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
                && let (Ok(buf), Ok(stride)) = (parts[2].parse::<u8>(), parts[3].parse::<u32>())
            {
                out.insert(buf, stride);
            }
        }
    }
    out
}

fn check_streams(key: &str, slot: u8, dynamic: bool, streams: &[u8], regs: &HashMap<u32, u32>) -> Result<()> {
    let mut buffers: Vec<(u8, bool)> = Vec::new();
    if slot != 0xFF {
        buffers.push((slot, dynamic));
    }
    for &s in streams {
        buffers.push(((s & !STREAM_BIND_STRIDE) - 1, (s & STREAM_BIND_STRIDE) != 0));
    }
    if buffers.is_empty() {
        return Ok(());
    }
    let strides = layout_strides(key);
    for (k, &(buffer, bind_stride)) in buffers.iter().enumerate() {
        let stride = if bind_stride {
            Some(0)
        } else {
            strides.get(&buffer).copied()
        };
        let held = regs.get(&(STREAM_FORMAT + 0x10 * (k as u32))).copied().unwrap_or(0) & STREAM_STRIDE;
        if stride.is_none_or(|s| s > STREAM_STRIDE || held != s) {
            return Err(LinkError::Invalid(format!(
                "vertex stream {k} of buffer {buffer} at stride {held}, its layout {stride:?}: {key}"
            )));
        }
    }
    Ok(())
}

fn decode_entries(
    pipeline: &[u8],
    count: usize,
    samplers: &[[u32; 8]],
    constexpr_tsc: &mut Vec<[u32; 8]>,
    tsc_index: &mut HashMap<[u32; 8], usize>,
    far_words: &[u32],
) -> Vec<PipelineEntry> {
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let e: [u8; 8] = if i < PACK_ENTRIES {
            pipeline[40 + 8 * i..48 + 8 * i].try_into().expect("8 bytes")
        } else {
            let fi = i - PACK_ENTRIES;
            let w0 = far_words[2 * fi].to_le_bytes();
            let w1 = far_words[2 * fi + 1].to_le_bytes();
            [w0[0], w0[1], w0[2], w0[3], w1[0], w1[1], w1[2], w1[3]]
        };
        let off = u16::from_le_bytes([e[0], e[1]]);
        let kind = e[2];
        let stage = e[3];
        let slot = e[4];
        let sampler_kind = e[5];
        let sampler = e[6];
        let pad = e[7];
        if kind == 5 {
            out.push(PipelineEntry::ColorTable { off });
        } else if matches!(kind, 8..=10) {
            out.push(PipelineEntry::Table { off, kind, stage });
        } else if kind == 1 {
            let residual_code = sampler_kind >> 2;
            out.push(PipelineEntry::Ubo {
                off,
                stage: if stage == 1 { 1 } else { 2 },
                slot,
                ssbo: (sampler_kind & 1) != 0,
                residual: (residual_code != 0).then(|| 0x700 + 4 * u32::from(residual_code - 1)),
                window: (u32::from(sampler) | (u32::from(pad) << 8)) * 16,
            });
        } else {
            let tex = if kind != 0 { Some((kind, slot)) } else { None };
            let (smp, compare) = if sampler_kind == 1 {
                (Some((1, sampler)), (pad & ENTRY_COMPARES) != 0)
            } else if sampler_kind == 2 {
                let header = samplers[usize::from(sampler)];
                let idx = *tsc_index.entry(header).or_insert_with(|| {
                    let next = constexpr_tsc.len();
                    constexpr_tsc.push(header);
                    next
                });
                (Some((2, idx as u8)), false)
            } else {
                (None, false)
            };
            out.push(PipelineEntry::Handle {
                off,
                stage: if stage == 1 { 1 } else { 2 },
                tex,
                smp,
                compare,
                array_view: (pad & ENTRY_ARRAY_VIEW) != 0,
            });
        }
    }
    out
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn pipeline_of(
    rec: &RawStateRecord,
    probe: &ProbeView,
    varying: &[u32],
    prog_index: &HashMap<String, usize>,
    constexpr_tsc: &mut Vec<[u32; 8]>,
    tsc_index: &mut HashMap<[u32; 8], usize>,
    varying_by_targets: &mut HashMap<u32, Vec<u32>>,
) -> Result<PipelineRecord> {
    let p = &rec.pipeline;
    if p.len() != PIPELINE_BYTES {
        return Err(LinkError::Invalid("pipeline record has an obsolete layout".into()));
    }
    let u16_at = |o: usize| u16::from_le_bytes([p[o], p[o + 1]]);
    let u32_at = |o: usize| u32::from_le_bytes(p[o..o + 4].try_into().expect("4 bytes"));
    let u64_at = |o: usize| u64::from_le_bytes(p[o..o + 8].try_into().expect("8 bytes"));

    let entries_low = usize::from(p[28]);
    let slot = p[30];
    let entries_high = usize::from(p[31]);
    let entries_count = entries_low | (entries_high << 8);
    let stride = u16_at(32);
    let set_bytes = u16_at(34);
    let flags = u32_at(36);

    let far_entries = entries_count.saturating_sub(PACK_ENTRIES);
    let masks = far_entries.div_ceil(32);
    if rec.far.len() != 2 * far_entries + 2 * masks {
        return Err(LinkError::Invalid(format!(
            "{}: a far block of {} words for {entries_count} entries",
            rec.key,
            rec.far.len()
        )));
    }

    let push_mask = u32_at(296);
    let mut push = Vec::new();
    if push_mask != 0 {
        for i in 0..32u32 {
            if ((push_mask >> i) & 1) != 0 {
                push.push((0x700 + 4 * i, u32_at(300 + 4 * (i as usize))));
            }
        }
    }

    let half_blend_color_word = u16_at(428);
    let storage_mask = p[432];
    let storage_write_mask = p[433];
    let storage_samples = p[434];
    let storage_tests = p[435];
    let mut storage_tic = [0u32; 8];
    for (i, st) in storage_tic.iter_mut().enumerate() {
        *st = u32_at(436 + 4 * i);
    }
    let depth_test_word = p[468];
    let stencil_test_word = p[469];
    let test_flags = p[470];
    let shadow_stencil_word = p[471];
    let depth_tic = u32_at(472);
    let stencil_tic = u32_at(476);

    let vertex_variants = usize::from(p[488]);
    let storage_blend_mask = p[490];
    let storage_blend_model = p[491];
    let bound_low = u32_at(492);
    let uniform: [u8; 32] = p[496..528].try_into().expect("32 bytes");
    let fetch_rung: [u8; 8] = p[528..536].try_into().expect("8 bytes");
    let memory_low = u32_at(540);

    let mut bound_words = vec![bound_low];
    let mut memory_words = vec![memory_low];
    for i in 0..masks {
        bound_words.push(rec.far[2 * far_entries + i]);
        memory_words.push(rec.far[2 * far_entries + masks + i]);
    }

    let root_table_word = [p[V20_AT], p[V20_AT + 1]];
    let status_word = [p[V20_AT + 2], p[V20_AT + 3]];
    let root_slots = [u32_at(V20_AT + 4), u32_at(V20_AT + 8)];
    let heap_arrays = usize::from(p[V20_AT + 12]);
    let heap_bytes = u32_at(V20_AT + 16);
    let mut heap = Vec::with_capacity(heap_arrays);
    for i in 0..heap_arrays {
        heap.push((u32_at(V20_AT + 20 + 8 * i), u32_at(V20_AT + 24 + 8 * i)));
    }
    let spill_high = u16_at(V22_AT + 34);
    let spill_entry = [
        u16::from(p[V20_AT + 84]) | ((spill_high & 0xFF) << 8),
        u16::from(p[V20_AT + 85]) | ((spill_high >> 8) << 8),
    ];
    let sw0 = usize::from(p[V20_AT + 86]);
    let sw1 = usize::from(p[V20_AT + 87]);
    let spill = [
        rec.spill[..sw0.min(rec.spill.len())].to_vec(),
        rec.spill[sw0.min(rec.spill.len())..(sw0 + sw1).min(rec.spill.len())].to_vec(),
    ];

    let runtime_head: [u8; 7] = p[V21_AT..V21_AT + 7].try_into().expect("7 bytes");
    let array_length_word = p[V21_AT + 7];
    let pulled_stride: [u8; 16] = p[V21_AT + 8..V21_AT + 24].try_into().expect("16 bytes");
    let raw_streams = &p[V21_AT + 24..V21_AT + 31];
    let zero_pos = raw_streams.iter().position(|&b| b == 0).unwrap_or(raw_streams.len());
    let vertex_streams = raw_streams[..zero_pos].to_vec();
    let viewport_count_word = p[V21_AT + 31];

    let mut written_format = [0u16; 16];
    for (i, wf) in written_format.iter_mut().enumerate() {
        *wf = u16_at(V22_AT + 2 * i);
    }
    let xr_slots = u16_at(V22_AT + 32);

    let stride_twin = u64_at(V24_AT + 8);
    let amplification_layer_word = p[V24_AT + 16];
    let amplification_viewport_word = p[V24_AT + 17];
    let guest_base_word: [u8; 4] = p[V36_AT..V36_AT + 4].try_into().expect("4 bytes");
    let guest_base_slot: [u8; 4] = p[V36_AT + 4..V36_AT + 8].try_into().expect("4 bytes");

    check_streams(&rec.key, slot, runtime_head[3] != 0, &vertex_streams, &probe.regs)?;

    let mut blob: Vec<(u32, u32)> = if storage_mask != 0 || shadow_stencil_word != 0 || (flags & 256) != 0 {
        let mut methods = varying.to_vec();
        if storage_mask != 0 || shadow_stencil_word != 0 {
            for m in [0x0210u32, 0x0754, 0x1224] {
                if !methods.contains(&m) {
                    methods.push(m);
                }
            }
            for i in 0..8u32 {
                if ((storage_mask >> i) & 1) != 0 {
                    let m = 0x1A00 + 4 * i;
                    if !methods.contains(&m) {
                        methods.push(m);
                    }
                }
            }
        }
        if (flags & 256) != 0 && !methods.contains(&0x1224) {
            methods.push(0x1224);
        }
        methods
            .into_iter()
            .filter(|&m| !PROGRAM_OFFSETS.contains(&m) && !is_foreign(m, probe.targets))
            .map(|m| (m, probe.regs.get(&m).copied().unwrap_or(0)))
            .collect()
    } else {
        let methods = varying_by_targets.entry(probe.targets).or_insert_with(|| {
            varying
                .iter()
                .copied()
                .filter(|&m| !PROGRAM_OFFSETS.contains(&m) && !is_foreign(m, probe.targets))
                .collect()
        });
        methods
            .iter()
            .map(|&m| (m, probe.regs.get(&m).copied().unwrap_or(0)))
            .collect()
    };
    blob.extend_from_slice(&rec.extras);

    let tess = if rec.tess_programs.len() >= 2 {
        [
            prog_index[&rec.tess_programs[0].program_id()] as u32,
            prog_index[&rec.tess_programs[1].program_id()] as u32,
        ]
    } else {
        [0, 0]
    };

    let entries = decode_entries(p, entries_count, &rec.samplers, constexpr_tsc, tsc_index, &rec.far);
    let variants = rec
        .variants
        .iter()
        .enumerate()
        .map(|(i, v)| PipelineVariant {
            stage: if i < vertex_variants { 0 } else { 1 },
            assume: v.assume,
            program: prog_index[&v.program.program_id()] as u32,
            memory: v.memory,
        })
        .collect();

    Ok(PipelineRecord {
        index: probe.index,
        key: rec.key.clone(),
        form: rec.form.clone(),
        tier: rec.tier,
        vs: prog_index[&probe.vs_pid] as u32,
        fs: probe.fs_pid.as_ref().map_or(NO_PROGRAM, |id| prog_index[id] as u32),
        blob,
        restore: rec.restore.clone(),
        tess,
        runtime_head,
        pulled_stride,
        stride_twin,
        amplification_layer_word,
        amplification_viewport_word,
        guest_base_word,
        guest_base_slot,
        push,
        push_metadata: (flags & 4) != 0,
        turn_refused: (flags & 64) != 0,
        raster_ordered: (flags & 256) != 0,
        half_blend_color_word,
        iterated: 0,
        iterated_name: rec.iterated.clone(),
        fs_image: rec.programs[1].image.clone(),
        xr_slots,
        storage_mask,
        storage_write_mask,
        storage_samples,
        storage_tests,
        storage_blend_mask,
        storage_blend_model,
        depth_test_word,
        stencil_test_word,
        test_flags,
        shadow_stencil_word,
        depth_tic,
        stencil_tic,
        storage_tic,
        set_size: set_bytes,
        entries,
        vb_slot: slot,
        vb_stride: if slot != 0xFF { stride } else { 0 },
        vertex_streams,
        array_length_word,
        viewport_count_word,
        fetch: (flags & 1) != 0,
        bound_words,
        memory_words,
        root_table_word,
        status_word,
        root_slots,
        heap,
        heap_bytes,
        spill_entry,
        spill,
        uniform,
        fetch_rung,
        written_format,
        variants,
    })
}
