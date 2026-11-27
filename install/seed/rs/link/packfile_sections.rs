// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap};

use crate::iterated_ucode::IteratedProgram;
use crate::kernels::ComputeFacts;
use crate::packfile::{PACK_ENTRIES, PACK_SPLIT_BYTES, PipelineRecord};
use crate::packfile_records::{entry_bytes, far_words, pipeline_bytes};
use crate::records::fnv1a64;
use crate::splits::SplitRecord;
use crate::{LinkError, Result};

fn twin_indices(pipes: &[PipelineRecord], hashes: &[u64], vnames: &[String]) -> Vec<u64> {
    let mut runs: HashMap<u64, Vec<usize>> = HashMap::new();
    for (at, _) in pipes.iter().enumerate() {
        if vnames[at].is_empty() {
            runs.entry(hashes[at]).or_default().push(at);
        }
    }
    pipes
        .iter()
        .map(|rec| {
            if rec.stride_twin == 0 {
                return 0;
            }
            let Some(ats) = runs.get(&rec.stride_twin) else {
                return 0;
            };
            let found = if rec.form == "free" || rec.form == "barrier" {
                ats.iter().copied().find(|&at| pipes[at].form == rec.form)
            } else {
                let plain: Vec<usize> = ats
                    .iter()
                    .copied()
                    .filter(|&at| pipes[at].form != "free" && pipes[at].form != "barrier")
                    .collect();
                plain
                    .iter()
                    .copied()
                    .find(|&at| pipes[at].storage_mask != 0)
                    .or_else(|| plain.first().copied())
            };
            found.map_or(0, |idx| (idx + 1) as u64)
        })
        .collect()
}

pub(crate) type PipelineSections = (Vec<u8>, Vec<u32>, Vec<u8>, Vec<u8>, usize, usize);

pub(crate) fn pipeline_sections(
    pipes: &[PipelineRecord],
    hashes: &[u64],
    vnames: &[String],
) -> Result<PipelineSections> {
    let mut strings = Vec::new();
    let push_string = |strings: &mut Vec<u8>, s: &str| -> u32 {
        let off = strings.len() as u32;
        strings.extend_from_slice(s.as_bytes());
        strings.push(0);
        off
    };

    let mut blob: Vec<u32> = Vec::new();
    let mut blobs: HashMap<&[(u32, u32)], u32> = HashMap::new();
    let mut pipe_bin = Vec::with_capacity(pipes.len() * 736);
    let mut variant_bin = Vec::new();
    let twins = twin_indices(pipes, hashes, vnames);
    let mut tier0_recorded = false;
    let mut tier0_strings = 0usize;
    let mut tier0_variants = 0usize;

    for (idx, rec) in pipes.iter().enumerate() {
        if !tier0_recorded && rec.tier != 0 {
            tier0_strings = strings.len();
            tier0_variants = variant_bin.len();
            tier0_recorded = true;
        }
        let blob_off = match blobs.get(rec.blob.as_slice()) {
            Some(&off) => off,
            None => {
                let off = blob.len() as u32;
                blobs.insert(rec.blob.as_slice(), off);
                for &(m, v) in &rec.blob {
                    blob.push(m);
                    blob.push(v);
                }
                off
            }
        };
        let variants_off = (variant_bin.len() / 12) as u32;
        if !rec.variants.is_empty() {
            let mut sorted_vars = rec.variants.clone();
            if sorted_vars.len() > 1 {
                sorted_vars.sort_by_key(|v| v.stage);
            }
            for v in &sorted_vars {
                variant_bin.extend_from_slice(&v.assume.to_le_bytes());
                variant_bin.extend_from_slice(&v.program.to_le_bytes());
                variant_bin.extend_from_slice(&v.memory.to_le_bytes());
            }
        }
        let key_head = rec
            .key
            .split_once(" | ")
            .map_or(rec.key.as_str(), |(h, _)| h)
            .trim_end();
        let key_off = push_string(&mut strings, key_head);
        let spill_at = blob.len() as u32;
        for words in &rec.spill {
            if !words.is_empty() {
                blob.extend_from_slice(words);
            }
        }
        let restore_at = blob.len() as u32;
        for &(m, v) in &rec.restore {
            blob.push(m);
            blob.push(v);
        }
        let far_at = blob.len();
        let ents = entry_bytes(&rec.entries)?;
        if rec.entries.len() > PACK_ENTRIES {
            blob.extend_from_slice(&far_words(rec, &ents));
        }
        let name_off = if vnames[idx].is_empty() {
            0
        } else {
            push_string(&mut strings, &vnames[idx])
        };
        let rec_bytes = pipeline_bytes(
            rec,
            key_off,
            blob_off,
            variants_off,
            name_off,
            hashes[idx],
            &vnames[idx],
            &ents[..8 * PACK_ENTRIES],
            spill_at,
            restore_at,
            far_at,
            twins[idx],
        )?;
        pipe_bin.extend_from_slice(&rec_bytes);
    }
    if !tier0_recorded {
        tier0_strings = strings.len();
        tier0_variants = variant_bin.len();
    }
    Ok((pipe_bin, blob, strings, variant_bin, tier0_strings, tier0_variants))
}

pub(crate) fn iterated_bytes(programs: &[IteratedProgram]) -> Vec<u8> {
    let mut out = Vec::with_capacity(programs.len() * 592);
    for p in programs {
        out.push(p.words.len() as u8);
        out.push(p.enable);
        out.push(p.constants.len() as u8);
        out.push(p.uniform_slot);
        out.extend_from_slice(&p.uniform_offset.to_le_bytes());
        out.push(p.uniform_words);
        out.push(0);
        out.extend_from_slice(&p.alpha_source.to_le_bytes());
        out.extend_from_slice(&p.alpha_dest.to_le_bytes());
        out.extend_from_slice(&fnv1a64(p.name.as_bytes()).to_le_bytes());
        out.extend_from_slice(&0u64.to_le_bytes());
        for j in 0..8u32 {
            let tri = p.constants.get(&j).copied().unwrap_or([0, 0, 0]);
            for h in tri {
                out.extend_from_slice(&h.to_le_bytes());
            }
        }
        for i in 0..128 {
            let w = p.words.get(i).copied().unwrap_or(0);
            out.extend_from_slice(&w.to_le_bytes());
        }
    }
    out
}

pub(crate) fn split_bytes(found: &[SplitRecord]) -> Vec<u8> {
    let mut out = Vec::with_capacity(found.len() * PACK_SPLIT_BYTES);
    for s in found {
        out.extend_from_slice(&s.threshold.to_le_bytes());
        out.extend_from_slice(&s.whole.to_le_bytes());
        out.extend_from_slice(&s.fs.to_le_bytes());
        out.extend_from_slice(&s.prologue.to_le_bytes());
        out.extend_from_slice(&s.frontier_bytes.to_le_bytes());
        out.push(s.frontier_cbuf);
        out.push(s.ubo_cbufs.len() as u8);
        out.push(s.shader_data_cbuf);
        out.push(s.prologue_cbufs);
        let mut ubo_buf = [0u8; 8];
        let n = s.ubo_cbufs.len().min(8);
        ubo_buf[..n].copy_from_slice(&s.ubo_cbufs[..n]);
        out.extend_from_slice(&ubo_buf);
        out.extend_from_slice(&s.batch_version.to_le_bytes());
        out.extend_from_slice(&s.batch_threads.to_le_bytes());
        for w in s.qmd {
            out.extend_from_slice(&w.to_le_bytes());
        }
        out.extend_from_slice(&s.cost_version.to_le_bytes());
        out.extend_from_slice(&s.cost_reserved.to_le_bytes());
        for w in s.cost_fields {
            out.extend_from_slice(&w.to_le_bytes());
        }
    }
    out
}

pub(crate) fn compute_facts_bytes(facts: &[ComputeFacts]) -> Vec<u8> {
    let mut sorted = facts.to_vec();
    sorted.sort_by_key(|f| f.hash);
    let mut out = Vec::with_capacity(sorted.len() * 24);
    for f in sorted {
        out.extend_from_slice(&f.hash.to_le_bytes());
        out.extend_from_slice(&f.max_threads.to_le_bytes());
        out.extend_from_slice(&f.execution_width.to_le_bytes());
        out.extend_from_slice(&f.static_memory.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
    }
    out
}

pub(crate) fn reflection_bytes(reflections: &BTreeMap<(bool, u64), Vec<u8>>) -> Vec<u8> {
    let render: Vec<(u64, &[u8])> = reflections
        .iter()
        .filter(|&((c, _), _)| !*c)
        .map(|(&(_, h), b)| (h, b.as_slice()))
        .collect();
    let compute: Vec<(u64, &[u8])> = reflections
        .iter()
        .filter(|&((c, _), _)| *c)
        .map(|(&(_, h), b)| (h, b.as_slice()))
        .collect();
    let mut out = Vec::new();
    out.extend_from_slice(&(render.len() as u32).to_le_bytes());
    out.extend_from_slice(&(compute.len() as u32).to_le_bytes());
    let at = 8 + 16 * (render.len() + compute.len());
    let mut entries = Vec::with_capacity(16 * (render.len() + compute.len()));
    let mut blobs = Vec::new();
    let mut placed: HashMap<&[u8], usize> = HashMap::new();
    for (h, b) in render.into_iter().chain(compute) {
        let off = match placed.get(b) {
            Some(&existing) => existing,
            None => {
                let pad = (8 - ((at + blobs.len()) % 8)) % 8;
                blobs.resize(blobs.len() + pad, 0);
                let pos = at + blobs.len();
                placed.insert(b, pos);
                blobs.extend_from_slice(b);
                pos
            }
        };
        entries.extend_from_slice(&h.to_le_bytes());
        entries.extend_from_slice(&(off as u32).to_le_bytes());
        entries.extend_from_slice(&(b.len() as u32).to_le_bytes());
    }
    out.extend_from_slice(&entries);
    out.extend_from_slice(&blobs);
    out
}

pub(crate) fn fast_key_bytes(
    fast_keys: &HashMap<u64, u64>,
    pipes: &[PipelineRecord],
    hashes: &[u64],
) -> Result<(Vec<u8>, usize)> {
    let mut runs: HashMap<u64, (u32, u32, u32)> = HashMap::new();
    let mut last: Option<(u32, u64)> = None;
    for (at, rec) in pipes.iter().enumerate() {
        let h = hashes[at];
        let order = (rec.tier, h);
        if let Some(prev) = last
            && order < prev
        {
            return Err(LinkError::Invalid(format!(
                "pipeline {at} ({h:016x}) after {:016x}: not in tier and key hash order",
                prev.1
            )));
        }
        runs.entry(h)
            .and_modify(|entry| entry.2 += 1)
            .or_insert((rec.tier, at as u32, 1));
        last = Some(order);
    }
    let mut rows: Vec<(u32, u64, u32, u32)> = fast_keys
        .iter()
        .filter_map(|(&fast, &h)| runs.get(&h).map(|&(t, start, count)| (t, fast, start, count)))
        .collect();
    rows.sort_unstable();
    let tier0_rows = rows.iter().filter(|r| r.0 == 0).count();
    let mut out = Vec::with_capacity(rows.len() * 16);
    for (_, fast, start, count) in rows {
        out.extend_from_slice(&fast.to_le_bytes());
        out.extend_from_slice(&start.to_le_bytes());
        out.extend_from_slice(&count.to_le_bytes());
    }
    Ok((out, tier0_rows))
}
