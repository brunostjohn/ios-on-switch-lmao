// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeSet, HashMap};

use crate::headers::{chip_facts, dropped_methods};
use crate::iterated_ucode::IteratedProgram;
use crate::packfile::{NO_PROGRAM, Pack, PipelineRecord, PipelineVariant, PlacedProgram, TIER_ALIGN};
use crate::segment_methods::{
    CT_WRITE, FLAG_BLEND_KILL, FLAG_KEPT_ACROSS_PASSES, FLAG_RESELECT_ROOT, HYBRID_ANTI_ALIAS_CONTROL,
    ITERATED_BLEND_OPTIMIZATION, NOOP_NEVER, NOOP_SOURCE_RGBA_0000, PIPELINE_STAGE_STRIDE,
    PIXEL_SHADER_INTERLOCK_CONTROL, PIXEL_STAGE_OFF, PIXEL_STAGE_SWITCH, PREFETCH, RASTER_SWITCH, SEGMENT_BIND_WORDS,
    SEGMENT_MAX_WORDS, SET_BINDLESS_TEXTURE, SET_GROUPS, SET_PATCH, SET_PIPELINE_PROGRAM_FS, SET_PIPELINE_PROGRAM_TESS,
    SET_PIPELINE_PROGRAM_VS, SET_PIPELINE_REGISTER_COUNT_FS, SET_PIPELINE_REGISTER_COUNT_TESS,
    SET_PIPELINE_REGISTER_COUNT_VS, bind_program_cbufs, choose_variant, color_formats, emit_iterated_blend,
    fetch_free_iterated, is_pass_write_method, iterated_blend_noop, last_written, legal_interlock, push_m,
};

pub fn slm_per_warp_of_header(heap: &[u8], prog_offset: usize) -> u32 {
    if prog_offset + 16 > heap.len() {
        return 0;
    }
    let sph1 = u32::from_le_bytes(heap[prog_offset + 4..prog_offset + 8].try_into().expect("4"));
    let sph2 = u32::from_le_bytes(heap[prog_offset + 8..prog_offset + 12].try_into().expect("4"));
    let sph3 = u32::from_le_bytes(heap[prog_offset + 12..prog_offset + 16].try_into().expect("4"));
    let per_thread = u64::from(sph1 & 0x00FF_FFFF) | (u64::from(sph2 & 0x00FF_FFFF) << 32);
    let crs = u64::from(sph3 & 0x00FF_FFFF);
    if per_thread == 0 && crs == 0 {
        return 0;
    }
    ((per_thread * 32 + crs + 0x1FF) & !0x1FF) as u32
}

#[allow(clippy::too_many_arguments)]
fn build_pipeline_stream(
    rec: &PipelineRecord,
    vs: &PlacedProgram,
    fs: Option<&PlacedProgram>,
    base_ct_write: [u32; 8],
    iterated_list: &[IteratedProgram],
    tess: Option<(&PlacedProgram, &PlacedProgram)>,
    dropped: &BTreeSet<u32>,
    base_blend: [u32; 6],
    base_blend_seen: u8,
) -> (Vec<u32>, u16) {
    let key_string = rec
        .key
        .split_once(" | ")
        .map_or(rec.key.as_str(), |(h, _)| h)
        .trim_end();
    let mut words = Vec::new();
    let mut writes = base_ct_write;
    let mut interlock = 0u32;
    let mut sample_control = 1u32;
    let mut kept = true;

    for &(m, v) in &rec.blob {
        if dropped.contains(&m) {
            continue;
        }
        if (CT_WRITE..CT_WRITE + 32).contains(&m) {
            writes[((m - CT_WRITE) >> 2) as usize] = v;
        }
        if m == PIXEL_SHADER_INTERLOCK_CONTROL {
            interlock = v;
            continue;
        }
        if m == HYBRID_ANTI_ALIAS_CONTROL {
            sample_control = v;
            continue;
        }
        if kept && is_pass_write_method(m) {
            kept = false;
        }
        push_m(&mut words, m, v);
    }
    push_m(
        &mut words,
        PIXEL_SHADER_INTERLOCK_CONTROL,
        legal_interlock(interlock, sample_control),
    );
    push_m(&mut words, HYBRID_ANTI_ALIAS_CONTROL, sample_control);
    let blend_kill = iterated_blend_noop(
        &rec.blob,
        &writes,
        key_string,
        rec.storage_mask,
        base_blend,
        base_blend_seen,
    );
    push_m(&mut words, ITERATED_BLEND_OPTIMIZATION, blend_kill);
    push_m(&mut words, SET_PIPELINE_PROGRAM_VS, vs.program as u32);
    push_m(&mut words, SET_PIPELINE_REGISTER_COUNT_VS, u32::from(vs.gprs));
    if let Some(fs_p) = fs {
        push_m(&mut words, SET_PIPELINE_PROGRAM_FS, fs_p.program as u32);
        push_m(&mut words, SET_PIPELINE_REGISTER_COUNT_FS, u32::from(fs_p.gprs));
        if fs_p.bound_textures != 0 {
            push_m(&mut words, SET_BINDLESS_TEXTURE, u32::from(fs_p.bound_textures - 1));
        }
    }
    let mut reselect = bind_program_cbufs(&mut words, vs, SET_GROUPS[0]);
    if let Some(fs_p) = fs {
        reselect |= bind_program_cbufs(&mut words, fs_p, SET_GROUPS[1]);
    }
    if let Some((t0, t1)) = tess {
        push_m(&mut words, SET_PATCH, 1);
        for (k, tp) in [t0, t1].into_iter().enumerate() {
            let stride = PIPELINE_STAGE_STRIDE * (k as u32);
            push_m(&mut words, SET_PIPELINE_PROGRAM_TESS + stride, tp.program as u32);
            push_m(
                &mut words,
                SET_PIPELINE_REGISTER_COUNT_TESS + stride,
                u32::from(tp.gprs),
            );
            reselect |= bind_program_cbufs(&mut words, tp, SET_GROUPS[2 + k]);
        }
    }
    if fs.is_none() {
        push_m(&mut words, PIXEL_STAGE_SWITCH, PIXEL_STAGE_OFF);
    }
    if key_string.contains(" rast=0") {
        push_m(&mut words, RASTER_SWITCH, 0);
    }
    if let Some((colour, it)) = fetch_free_iterated(rec, iterated_list) {
        emit_iterated_blend(&mut words, it, colour, color_formats(key_string)[colour]);
    }
    let kills = last_written(&words, ITERATED_BLEND_OPTIMIZATION, NOOP_NEVER) == NOOP_SOURCE_RGBA_0000;
    let flags = (if kept { FLAG_KEPT_ACROSS_PASSES } else { 0 })
        | (if reselect { FLAG_RESELECT_ROOT } else { 0 })
        | (if kills { FLAG_BLEND_KILL } else { 0 });
    (words, flags)
}

fn encode_table(
    base: &[(u32, u16, u16)],
    var_bases: &[u32],
    var_combos: &[(u32, u16, u16)],
    words: (u32, u32),
) -> Vec<u8> {
    let mut out = Vec::with_capacity(16 + base.len() * 12 + var_combos.len() * 8);
    out.extend_from_slice(&(base.len() as u32).to_le_bytes());
    out.extend_from_slice(&(var_combos.len() as u32).to_le_bytes());
    out.extend_from_slice(&words.0.to_le_bytes());
    out.extend_from_slice(&words.1.to_le_bytes());
    for &(off, count, flags) in base {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&count.to_le_bytes());
        out.extend_from_slice(&flags.to_le_bytes());
    }
    for &vb in var_bases {
        out.extend_from_slice(&vb.to_le_bytes());
    }
    for &(off, count, flags) in var_combos {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&count.to_le_bytes());
        out.extend_from_slice(&flags.to_le_bytes());
    }
    out
}

pub fn build_tier0_segments(pack: &mut Pack, report: &mut Vec<String>) {
    let tier0_pipes = pack.pipes.iter().filter(|r| r.tier == 0).count();
    let tier0_progs = pack.tier0_programs.unwrap_or(pack.programs.len());
    if tier0_pipes == 0 || tier0_progs == 0 {
        pack.segments.clear();
        pack.tier0_slm_per_warp = 0;
        return;
    }

    let mut base_ct_write = [0u32; 8];
    let mut base_blend = [0u32; 6];
    let mut base_blend_seen = 0u8;
    for &(sc, m, d, _) in &pack.init {
        let m32 = u32::from(m);
        if sc == 0 && (0x1E04..=0x1E18).contains(&m32) {
            let idx = ((m32 - 0x1E04) >> 2) as usize;
            base_blend[idx] = d;
            base_blend_seen |= 1u8 << idx;
        }
    }
    for (&m, &v) in &pack.baseline {
        if (CT_WRITE..CT_WRITE + 32).contains(&m) {
            base_ct_write[((m - CT_WRITE) >> 2) as usize] = v;
        } else if (0x1E04..=0x1E18).contains(&m) {
            let idx = ((m - 0x1E04) >> 2) as usize;
            base_blend[idx] = v;
            base_blend_seen |= 1u8 << idx;
        }
    }

    let dropped = dropped_methods();
    let facts_base = chip_facts();
    let mut seg_blob: Vec<u8> = Vec::new();
    let mut seg_pool: HashMap<Vec<u8>, u32> = HashMap::new();

    let mut place_stream = |words: &[u32], flags: u16| -> (u32, u16, u16) {
        let count = words.len();
        if count <= SEGMENT_BIND_WORDS || count > SEGMENT_MAX_WORDS {
            return (0, 0, 0);
        }
        let mut raw = Vec::with_capacity(words.len() * 4);
        for &w in words {
            raw.extend_from_slice(&w.to_le_bytes());
        }
        let off = match seg_pool.get(&raw) {
            Some(&existing) => existing,
            None => {
                let pos = seg_blob.len() as u32;
                seg_blob.extend_from_slice(&raw);
                let pad = (16 - (seg_blob.len() % 16)) % 16;
                seg_blob.resize(seg_blob.len() + pad, 0);
                seg_pool.insert(raw, pos);
                pos
            }
        };
        (off, count as u16, flags)
    };

    let mut base_entries: Vec<(u32, u16, u16)> = Vec::with_capacity(tier0_pipes);
    let mut var_bases: Vec<u32> = Vec::with_capacity(tier0_pipes);
    let mut var_combos: Vec<(u32, u16, u16)> = Vec::new();
    let mut gfx_prog_indices: BTreeSet<usize> = BTreeSet::new();

    for rec in &pack.pipes[..tier0_pipes] {
        let vs_idx = rec.vs as usize;
        let fs_idx = rec.fs;
        if vs_idx >= tier0_progs || (fs_idx != NO_PROGRAM && (fs_idx as usize) >= tier0_progs) {
            base_entries.push((0, 0, 0));
            var_bases.push(0xFFFF_FFFF);
            continue;
        }
        let vs = &pack.programs[vs_idx];
        let fs = if fs_idx == NO_PROGRAM {
            None
        } else {
            Some(&pack.programs[fs_idx as usize])
        };
        gfx_prog_indices.insert(vs_idx);
        if fs_idx != NO_PROGRAM {
            gfx_prog_indices.insert(fs_idx as usize);
        }
        let tess = if rec.runtime_head[0] != 0 {
            let t0 = rec.tess[0] as usize;
            let t1 = rec.tess[1] as usize;
            if t0 >= tier0_progs || t1 >= tier0_progs {
                base_entries.push((0, 0, 0));
                var_bases.push(0xFFFF_FFFF);
                continue;
            }
            gfx_prog_indices.insert(t0);
            gfx_prog_indices.insert(t1);
            Some((&pack.programs[t0], &pack.programs[t1]))
        } else {
            None
        };

        let (words, flags) = build_pipeline_stream(
            rec,
            vs,
            fs,
            base_ct_write,
            &pack.iterated,
            tess,
            dropped,
            base_blend,
            base_blend_seen,
        );
        base_entries.push(place_stream(&words, flags));

        if rec.variants.is_empty() {
            var_bases.push(0xFFFF_FFFF);
            continue;
        }
        let v0: Vec<&PipelineVariant> = rec.variants.iter().filter(|v| v.stage == 0).collect();
        let v1: Vec<&PipelineVariant> = rec.variants.iter().filter(|v| v.stage == 1).collect();
        for v in &rec.variants {
            if (v.program as usize) < tier0_progs {
                gfx_prog_indices.insert(v.program as usize);
            }
        }
        let (c0, c1) = (v0.len(), v1.len());
        if c0 == 0 && c1 == 0 {
            var_bases.push(0xFFFF_FFFF);
            continue;
        }
        var_bases.push(var_combos.len() as u32);
        for vi in 0..=c0 {
            for fi in 0..=c1 {
                if vi == c0 && fi == c1 {
                    continue;
                }
                let facts =
                    facts_base | if vi < c0 { v0[vi].assume } else { 0 } | if fi < c1 { v1[fi].assume } else { 0 };
                if choose_variant(&v0, facts) != vi || choose_variant(&v1, facts) != fi {
                    var_combos.push((0, 0, 0));
                    continue;
                }
                let p0_idx = if vi < c0 { v0[vi].program as usize } else { vs_idx };
                let p1_idx = if fi < c1 { v1[fi].program } else { fs_idx };
                if p0_idx >= tier0_progs || (p1_idx != NO_PROGRAM && (p1_idx as usize) >= tier0_progs) {
                    var_combos.push((0, 0, 0));
                    continue;
                }
                let p0 = &pack.programs[p0_idx];
                let p1 = if p1_idx == NO_PROGRAM {
                    None
                } else {
                    Some(&pack.programs[p1_idx as usize])
                };
                let (vw, vf) = build_pipeline_stream(
                    rec,
                    p0,
                    p1,
                    base_ct_write,
                    &pack.iterated,
                    None,
                    dropped,
                    base_blend,
                    base_blend_seen,
                );
                var_combos.push(place_stream(&vw, vf));
            }
        }
    }

    let mut max_slm = 0u32;
    for idx in gfx_prog_indices {
        max_slm = max_slm.max(slm_per_warp_of_header(&pack.heap, pack.programs[idx].program));
    }
    pack.tier0_slm_per_warp = max_slm;

    let old_tier0_heap = pack.tier0_heap.unwrap_or(pack.heap.len());
    let mut words_span = (0u32, 0u32);
    if !seg_blob.is_empty() {
        let tier0_prog_end = pack.programs[..tier0_progs]
            .iter()
            .map(|pr| pr.heap_offset + pr.bytes)
            .max()
            .unwrap_or(0);
        let seg_start = (tier0_prog_end + PREFETCH + 15) & !15;
        let seg_end = seg_start + seg_blob.len();
        words_span = (seg_start as u32, seg_blob.len() as u32);
        let new_tier0_heap = old_tier0_heap.max((seg_end + TIER_ALIGN - 1) & !(TIER_ALIGN - 1));
        let delta = new_tier0_heap - old_tier0_heap;
        if delta == 0 {
            if pack.heap.len() < new_tier0_heap {
                pack.heap.resize(new_tier0_heap, 0);
            }
            pack.heap[seg_start..seg_end].copy_from_slice(&seg_blob);
        } else {
            let tier1_tail = pack.heap[old_tier0_heap..].to_vec();
            let mut new_heap = pack.heap[..seg_start.min(pack.heap.len())].to_vec();
            new_heap.resize(seg_start, 0);
            new_heap.extend_from_slice(&seg_blob);
            new_heap.resize(new_tier0_heap, 0);
            new_heap.extend_from_slice(&tier1_tail);
            pack.heap = new_heap;
            if pack.tier0_heap.is_some() {
                pack.tier0_heap = Some(new_tier0_heap);
            }
            for pr in &mut pack.programs {
                if pr.heap_offset >= old_tier0_heap {
                    pr.heap_offset += delta;
                    pr.program += delta;
                }
            }
        }
        let seg_off = seg_start as u32;
        for entry in &mut base_entries {
            if entry.1 != 0 {
                entry.0 += seg_off;
            }
        }
        for entry in &mut var_combos {
            if entry.1 != 0 {
                entry.0 += seg_off;
            }
        }
    }

    let valid_combos = var_combos.iter().filter(|e| e.1 != 0).count();
    pack.segments = encode_table(&base_entries, &var_bases, &var_combos, words_span);
    report.push(format!(
        "tier 0 state segments: {} pipelines, {valid_combos} variant combos, {} distinct ({} KiB), tier0 slm {max_slm} B/warp",
        base_entries.len(),
        seg_pool.len(),
        seg_blob.len() / 1024
    ));
}
