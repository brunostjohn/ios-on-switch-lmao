// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::PathBuf;

use crate::archives;
use crate::fetchfree;
use crate::headers::{DEFAULT_DRAW_CONTEXT, apply_draw_context};
use crate::kernels;
use crate::packfile::{Pack, PipelineRecord, PlacedProgram, TIER_ALIGN, key_hash_str, place_graphics, variant_name};
use crate::packfile_encode::{pack_bytes, sections_line, stamp_pack_bytes, write_layouts, write_programs};
use crate::pipeline_build::pipeline_of;
use crate::probelog::{
    load_init, load_init_records, make_probes, set_tiers, split_identities, split_registers, variant_report,
};
use crate::records::{RawProgram, fast_keys, read_records};
use crate::segments;
use crate::splits::{self, SplitRecord};
use crate::{LinkError, Result};

const PREFETCH: usize = 0x1000;

#[derive(Clone, Debug)]
pub struct LinkConfig {
    pub buckets: Vec<PathBuf>,
    pub out_dir: PathBuf,
    pub context_log: Option<PathBuf>,
    pub draw_context: Option<PathBuf>,
    pub archives: Option<PathBuf>,
    pub corpus_report: Option<PathBuf>,
    pub stack: Option<u64>,
    pub write_debug_outputs: bool,
}

#[derive(Clone, Debug)]
pub struct LinkOutput {
    pub pack_bin: Vec<u8>,
    pub report: String,
    pub pipelines: usize,
    pub kernels: usize,
    pub programs: usize,
}

pub fn link_buckets(cfg: &LinkConfig) -> Result<LinkOutput> {
    let context_path = match &cfg.context_log {
        Some(p) => p.clone(),
        None => cfg
            .buckets
            .iter()
            .map(|d| d.join("context.log"))
            .find(|p| p.is_file())
            .ok_or_else(|| LinkError::Invalid("no context.log found in any bucket".into()))?,
    };
    let (_, post0) = load_init(&context_path)?;
    let draw_ctx_text = match &cfg.draw_context {
        Some(p) => fs::read_to_string(p)?,
        None => DEFAULT_DRAW_CONTEXT.to_owned(),
    };
    let draw_ctx = apply_draw_context(&post0, &draw_ctx_text);

    let mut states = Vec::new();
    let mut refused = Vec::new();
    let mut seen_state: HashSet<(String, String)> = HashSet::new();
    let mut all_fast = Vec::new();
    let mut all_kernels = Vec::new();
    let mut reflections: BTreeMap<(bool, u64), Vec<u8>> = BTreeMap::new();

    for dir in &cfg.buckets {
        let rec_path = if dir.is_file() {
            dir.clone()
        } else {
            dir.join("records.bin")
        };
        let bucket = read_records(&rec_path)?;
        all_fast.extend(bucket.fast);
        for (k, v) in bucket.reflections {
            reflections.entry(k).or_insert(v);
        }
        all_kernels.extend(bucket.kernels);
        for mut rec in bucket.states {
            let flags = if rec.outcome == 0 && rec.pipeline.len() >= 40 {
                u32::from_le_bytes(rec.pipeline[36..40].try_into().expect("4 bytes"))
            } else {
                0
            };
            let form = if (flags & 32) != 0 {
                "free"
            } else if (flags & 2) != 0 {
                "barrier"
            } else {
                ""
            };
            if !seen_state.insert((form.to_owned(), rec.key.clone())) {
                continue;
            }
            if rec.outcome != 0 {
                refused.push(rec);
            } else {
                rec.form = form.to_owned();
                states.push(rec);
            }
        }
    }

    split_identities(&mut states);
    let (fast, tiers) = fast_keys(&all_fast)?;
    let state_hashes = set_tiers(&mut states, &tiers)?;
    let probes = make_probes(&states, &draw_ctx);

    let mut programs: HashMap<String, RawProgram> = HashMap::new();
    let mut program_order: Vec<String> = Vec::new();
    let mut register_program = |pid: String, prog: RawProgram| {
        if !programs.contains_key(&pid) {
            program_order.push(pid.clone());
        }
        programs.insert(pid, prog);
    };
    for rec in &states {
        for p in &rec.programs {
            if !p.image.is_empty() {
                register_program(p.program_id(), p.clone());
            }
        }
    }
    let mut found_splits: HashMap<String, SplitRecord> = HashMap::new();
    for rec in &states {
        if let Some(split) = &rec.split {
            let fs_id = split.programs[0].program_id();
            let pro_id = split.programs[1].program_id();
            register_program(fs_id.clone(), split.programs[0].clone());
            register_program(pro_id.clone(), split.programs[1].clone());
            found_splits.insert(
                rec.programs[1].program_id(),
                splits::of_record(&split.record, fs_id, pro_id)?,
            );
        }
    }

    let mut report = vec![format!(
        "{} states made directly, {} records, {} refused",
        states.len() + refused.len(),
        states.len(),
        refused.len()
    )];
    report.extend(variant_report(&states));

    let (varying, baseline) = split_registers(&probes, &post0);
    let varying_hex: Vec<String> = varying.iter().map(|m| format!("{m:x}")).collect();
    report.push(format!(
        "{} per-pipeline registers: {}",
        varying.len(),
        varying_hex.join(" ")
    ));
    report.push(format!("{} baseline registers over post-init", baseline.len()));

    let mut heap: Vec<u8> = Vec::new();
    let mut prog_index: HashMap<String, usize> = HashMap::new();
    let mut storage_index: HashMap<String, usize> = HashMap::new();
    let mut prog_recs: Vec<PlacedProgram> = Vec::new();
    let mut split_list: Vec<SplitRecord> = Vec::new();
    let mut split_done: HashSet<String> = HashSet::new();
    let mut tier0_programs = 0usize;
    let mut tier0_heap = 0usize;

    for tier in [0u32, 1u32] {
        let group_idx: Vec<usize> = states
            .iter()
            .enumerate()
            .filter(|(_, r)| r.tier == tier)
            .map(|(i, _)| i)
            .collect();
        report.push(format!("tier {tier}: {} states", group_idx.len()));

        let mut used_generic: Vec<String> = Vec::new();
        let mut seen_generic: HashSet<String> = HashSet::new();
        for &i in &group_idx {
            if seen_generic.insert(probes[i].vs_pid.clone()) {
                used_generic.push(probes[i].vs_pid.clone());
            }
            if let Some(fs_pid) = &probes[i].fs_pid
                && seen_generic.insert(fs_pid.clone())
            {
                used_generic.push(fs_pid.clone());
            }
        }
        for pid in &used_generic {
            place_graphics(
                pid,
                &programs,
                &mut heap,
                &mut prog_index,
                &mut storage_index,
                &mut prog_recs,
            );
        }

        let mut var_count = 0usize;
        let mut tess_count = 0usize;
        for &i in &group_idx {
            let rec = &states[i];
            if !rec.variants.is_empty() {
                var_count += rec.variants.len();
                for v in &rec.variants {
                    let pid = v.program.program_id();
                    if !programs.contains_key(&pid) {
                        program_order.push(pid.clone());
                        programs.insert(pid.clone(), v.program.clone());
                    }
                    place_graphics(
                        &pid,
                        &programs,
                        &mut heap,
                        &mut prog_index,
                        &mut storage_index,
                        &mut prog_recs,
                    );
                }
            }
            if !rec.tess_programs.is_empty() {
                tess_count += 1;
                for tp in &rec.tess_programs {
                    let pid = tp.program_id();
                    if !programs.contains_key(&pid) {
                        program_order.push(pid.clone());
                        programs.insert(pid.clone(), tp.clone());
                    }
                    place_graphics(
                        &pid,
                        &programs,
                        &mut heap,
                        &mut prog_index,
                        &mut storage_index,
                        &mut prog_recs,
                    );
                }
            }
        }
        report.push(format!(
            "{var_count} program variants, {tess_count} tessellation states"
        ));

        let mut fresh_splits: Vec<String> = Vec::new();
        for &i in &group_idx {
            if !states[i].programs[1].image.is_empty() {
                let pid = states[i].programs[1].program_id();
                if split_done.insert(pid.clone()) {
                    fresh_splits.push(pid);
                }
            }
        }
        split_list.extend(splits::lay_out(
            &fresh_splits,
            &found_splits,
            &programs,
            &mut heap,
            &mut prog_index,
            &mut storage_index,
            &mut prog_recs,
            &mut report,
        ));
        if tier == 0 && !heap.is_empty() {
            let pad = (TIER_ALIGN - ((heap.len() + PREFETCH) % TIER_ALIGN)) % TIER_ALIGN + PREFETCH;
            heap.resize(heap.len() + pad, 0);
            tier0_programs = prog_recs.len();
            tier0_heap = heap.len();
        }
    }
    split_list.sort_by_key(|s| s.whole);
    let with_data = prog_recs.iter().filter(|p| p.data_bytes != 0).count();
    report.push(format!(
        "{} programs ({with_data} with shader data), shader heap image {} KiB",
        prog_recs.len(),
        heap.len() / 1024
    ));

    let mut constexpr_tsc: Vec<[u32; 8]> = Vec::new();
    let mut tsc_index: HashMap<[u32; 8], usize> = HashMap::new();
    let mut varying_by_targets: HashMap<u32, Vec<u32>> = HashMap::new();
    let mut pipe_pairs: Vec<(PipelineRecord, u64)> = Vec::with_capacity(states.len());
    for (i, rec) in states.iter().enumerate() {
        let pipe = pipeline_of(
            rec,
            &probes[i],
            &varying,
            &prog_index,
            &mut constexpr_tsc,
            &mut tsc_index,
            &mut varying_by_targets,
        )?;
        pipe_pairs.push((pipe, state_hashes[i]));
    }
    pipe_pairs.sort_by_key(|(p, h)| (p.tier, *h));
    let pipes: Vec<PipelineRecord> = pipe_pairs.into_iter().map(|(p, _)| p).collect();
    let (pipes, iterated) = fetchfree::attach(pipes, &mut report)?;

    let mut kernel_progs: HashMap<String, RawProgram> = HashMap::new();
    for krec in &mut all_kernels {
        if krec.outcome == 0
            && let Some(prog) = krec.program.take()
        {
            let pid = prog.program_id();
            krec.program_id = pid.clone();
            kernel_progs.insert(pid, prog);
        }
    }
    let (mut computes, skipped_kernels) = kernels::from_records(&all_kernels, &mut constexpr_tsc, &mut tsc_index);
    kernels::lay_out(&mut computes, &kernel_progs, &mut heap, &mut prog_index, &mut prog_recs);
    report.push(format!(
        "{} compute kernels, {} dispatch shapes left out",
        computes.len(),
        skipped_kernels.len()
    ));
    for (k, why) in &skipped_kernels {
        report.push(format!("  skipped kernel {k}: {why}"));
    }
    let compute_facts = kernels::state_facts(&all_kernels)?;
    report.push(format!("{} compute states answered", compute_facts.len()));

    let init_recs = load_init_records(&context_path)?;
    let inventory_text = match &cfg.archives {
        Some(p) => fs::read_to_string(p)?,
        None => String::new(),
    };
    let declared = archives::declared(&inventory_text, &archives::refused_keys(&states, &refused));
    let kernel_keys: Vec<String> = computes.iter().map(|k| k.key.clone()).collect();
    let held = archives::held_kernels(&kernel_keys);
    report.push(archives::report_line(&declared, &held));

    let render_refl = reflections.keys().filter(|(c, _)| !*c).count();
    let compute_refl = reflections.keys().filter(|(c, _)| *c).count();
    let distinct_refl: HashSet<&[u8]> = reflections.values().map(Vec::as_slice).collect();
    report.push(format!(
        "{render_refl} render and {compute_refl} compute reflections, {} distinct",
        distinct_refl.len()
    ));

    let mut pack = Pack {
        pipes,
        programs: prog_recs,
        constexpr_tsc,
        baseline,
        init: init_recs,
        heap,
        kernels: computes,
        splits: split_list,
        archives: archives::section_bytes(&declared, &held),
        iterated,
        compute_facts,
        reflections,
        fast_keys: fast,
        tier0_programs: Some(tier0_programs),
        tier0_heap: Some(tier0_heap),
        segments: Vec::new(),
        tier0_slm_per_warp: 0,
    };
    segments::build_tier0_segments(&mut pack, &mut report);

    let final_hashes: Vec<u64> = pack
        .pipes
        .iter()
        .map(|r| key_hash_str(&r.key, variant_name(&r.key)))
        .collect();
    let (mut data, blob) = pack_bytes(&pack, Some(&final_hashes))?;
    if let Some(stack) = cfg.stack {
        stamp_pack_bytes(&mut data, stack)?;
    }

    fs::create_dir_all(&cfg.out_dir)?;
    fs::write(cfg.out_dir.join("pack.bin"), &data)?;
    if cfg.write_debug_outputs {
        write_layouts(&cfg.out_dir.join("layouts.txt"), &pack)?;
        let ordered_progs: Vec<(String, &RawProgram)> = program_order
            .iter()
            .filter_map(|id| programs.get(id).map(|p| (id.clone(), p)))
            .collect();
        write_programs(&cfg.out_dir.join("programs.bin"), &ordered_progs)?;
    }

    let unshared: usize = pack.pipes.iter().map(|p| p.blob.len() * 2).sum();
    report.push(format!(
        "pack.bin {} KiB: {} pipelines, {} kernels, {} programs, {} splits, {} constexpr samplers, {} blob dwords ({unshared} unshared), {} baseline regs",
        data.len() / 1024,
        pack.pipes.len(),
        pack.kernels.len(),
        pack.programs.len(),
        pack.splits.len(),
        pack.constexpr_tsc.len(),
        blob.len(),
        pack.baseline.len()
    ));
    report.push(sections_line(&data));
    for rec in &refused {
        let short: String = rec.key.chars().take(150).collect();
        report.push(format!("  refused ({}): {short}", rec.why));
    }
    let mut report_text = report.join("\n") + "\n";
    if let Some(corpus_rep) = &cfg.corpus_report {
        report_text.push_str(&fs::read_to_string(corpus_rep)?);
    }
    fs::write(cfg.out_dir.join("report.txt"), &report_text)?;

    Ok(LinkOutput {
        pack_bin: data,
        report: report_text,
        pipelines: pack.pipes.len(),
        kernels: pack.kernels.len(),
        programs: pack.programs.len(),
    })
}
