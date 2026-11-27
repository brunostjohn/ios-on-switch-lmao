// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::Path;

use sha2::{Digest, Sha256};

use crate::headers::chip_facts;
use crate::packfile::{
    MAGIC, NO_PROGRAM, PACK_SPLIT_BYTES, PROGRAM_BYTES, PROGRAM_CBUFS, Pack, PipelineEntry, SECTIONS, TIER_ALIGN,
    VERSION, key_hash_str, variant_name,
};
use crate::packfile_records::{far_at_bytes, kernel_bytes};
use crate::packfile_sections::{
    compute_facts_bytes, fast_key_bytes, iterated_bytes, pipeline_sections, reflection_bytes, split_bytes,
};
use crate::records::fnv1a64;
use crate::{LinkError, Result};

const WHOLE_SECTIONS: [usize; 6] = [2, 3, 4, 5, 12, 16];
const STACK_OFFSET: usize = 8 + 16 + 16 * SECTIONS;

const SECTION_NAMES: [&str; SECTIONS] = [
    "programs",
    "pipelines",
    "samplers",
    "baseline",
    "init",
    "blob",
    "strings",
    "heap",
    "kernels",
    "splits",
    "variants",
    "archives",
    "iterated",
    "compute states",
    "reflections",
    "fast keys",
    "segments",
];

pub fn rules_identity(pack: &Pack) -> [u8; 16] {
    let mut varying: BTreeSet<u32> = BTreeSet::new();
    for rec in &pack.pipes {
        for &(m, _) in &rec.blob {
            varying.insert(m);
        }
    }
    let mut context: BTreeMap<u32, u32> = BTreeMap::new();
    for &(subc, method, data, reloc) in &pack.init {
        if subc == 0 && reloc == 0 && data != 0 {
            context.insert(u32::from(method), data);
        } else if subc == 0 {
            context.remove(&u32::from(method));
        }
    }
    let mut bytes = Vec::with_capacity((1 + varying.len() + 2 + 2 * (pack.baseline.len() + context.len())) * 4);
    bytes.extend_from_slice(&(varying.len() as u32).to_le_bytes());
    for m in varying {
        bytes.extend_from_slice(&m.to_le_bytes());
    }
    for regs in [&pack.baseline, &context] {
        bytes.extend_from_slice(&(regs.len() as u32).to_le_bytes());
        for (&m, &v) in regs {
            bytes.extend_from_slice(&m.to_le_bytes());
            bytes.extend_from_slice(&v.to_le_bytes());
        }
    }
    let digest = Sha256::digest(&bytes);
    digest[..16].try_into().expect("16 bytes")
}

pub fn pack_bytes(pack: &Pack, hashes_opt: Option<&[u64]>) -> Result<(Vec<u8>, Vec<u32>)> {
    let vnames: Vec<String> = pack.pipes.iter().map(|r| variant_name(&r.key).to_owned()).collect();
    let computed_hashes: Vec<u64>;
    let hashes: &[u64] = match hashes_opt {
        Some(h) => h,
        None => {
            computed_hashes = pack
                .pipes
                .iter()
                .zip(vnames.iter())
                .map(|(r, vn)| key_hash_str(&r.key, vn))
                .collect();
            &computed_hashes
        }
    };
    let (pipe_bin, mut blob, mut strings, variant_bin, tier0_strings, tier0_variants) =
        pipeline_sections(&pack.pipes, hashes, &vnames)?;

    let mut sorted_kernels: Vec<&crate::kernels::KernelRecord> = pack.kernels.iter().collect();
    sorted_kernels.sort_by_key(|k| fnv1a64(k.key.as_bytes()));
    let mut kernel_bin = Vec::with_capacity(sorted_kernels.len() * 1024);
    for k in sorted_kernels {
        kernel_bin.extend_from_slice(&kernel_bytes(k, strings.len() as u32, blob.len() as u32));
        blob.extend_from_slice(&k.spill);
        strings.extend_from_slice(k.key.as_bytes());
        strings.push(0);
    }

    let mut prog_bin = Vec::with_capacity(PROGRAM_BYTES * pack.programs.len());
    for pr in &pack.programs {
        let mut types = [0u8; PROGRAM_CBUFS];
        let n = pr.cbufs.len().min(PROGRAM_CBUFS);
        types[..n].copy_from_slice(&pr.cbufs[..n]);
        let mut far = [0u8; 3];
        if pr.far_ubos.iter().any(|&v| v != 0) {
            far = far_at_bytes(blob.len() + 1)?;
            let mut table = [0u16; PROGRAM_CBUFS];
            let fn_len = pr.far_ubos.len().min(PROGRAM_CBUFS);
            table[..fn_len].copy_from_slice(&pr.far_ubos[..fn_len]);
            for pair in table.as_chunks::<2>().0 {
                blob.push(u32::from(pair[0]) | (u32::from(pair[1]) << 16));
            }
        }
        prog_bin.extend_from_slice(&(pr.heap_offset as u32).to_le_bytes());
        prog_bin.extend_from_slice(&(pr.program as u32).to_le_bytes());
        prog_bin.extend_from_slice(&(pr.bytes as u32).to_le_bytes());
        prog_bin.extend_from_slice(&(pr.data_offset as u32).to_le_bytes());
        prog_bin.extend_from_slice(&(pr.data_bytes as u32).to_le_bytes());
        prog_bin.push(pr.cbufs.len() as u8);
        prog_bin.extend_from_slice(&types);
        prog_bin.push(pr.gprs);
        prog_bin.push(pr.bound_textures);
        prog_bin.extend_from_slice(&far);
        prog_bin.extend_from_slice(&pr.facts.to_le_bytes());
    }

    let mut tsc_bin = Vec::with_capacity(pack.constexpr_tsc.len() * 32);
    for hdr in &pack.constexpr_tsc {
        for &w in hdr {
            tsc_bin.extend_from_slice(&w.to_le_bytes());
        }
    }
    let mut base_bin = Vec::with_capacity(pack.baseline.len() * 8);
    for (&m, &v) in &pack.baseline {
        base_bin.extend_from_slice(&m.to_le_bytes());
        base_bin.extend_from_slice(&v.to_le_bytes());
    }
    let mut init_bin = Vec::with_capacity(pack.init.len() * 12);
    for &(sc, m, d, rc) in &pack.init {
        init_bin.extend_from_slice(&sc.to_le_bytes());
        init_bin.extend_from_slice(&m.to_le_bytes());
        init_bin.extend_from_slice(&d.to_le_bytes());
        init_bin.extend_from_slice(&rc.to_le_bytes());
    }
    let mut blob_bin = Vec::with_capacity(blob.len() * 4);
    for &w in &blob {
        blob_bin.extend_from_slice(&w.to_le_bytes());
    }
    let (fast_bin, fast_rows) = fast_key_bytes(&pack.fast_keys, &pack.pipes, hashes)?;

    let sections: [Vec<u8>; SECTIONS] = [
        prog_bin,
        pipe_bin,
        tsc_bin,
        base_bin,
        init_bin,
        blob_bin,
        strings,
        pack.heap.clone(),
        kernel_bin,
        split_bytes(&pack.splits),
        variant_bin,
        pack.archives.clone(),
        iterated_bytes(&pack.iterated),
        compute_facts_bytes(&pack.compute_facts),
        reflection_bytes(&pack.reflections),
        fast_bin,
        pack.segments.clone(),
    ];

    let tier0_pipes = pack.pipes.iter().filter(|r| r.tier == 0).count();
    let tier0_progs = pack.tier0_programs.unwrap_or(pack.programs.len());
    let tier0_heap = pack.tier0_heap.unwrap_or(pack.heap.len());
    let mut tier0 = [0u64; SECTIONS];
    for i in 0..SECTIONS {
        if WHOLE_SECTIONS.contains(&i) {
            tier0[i] = sections[i].len() as u64;
        }
    }
    tier0[0] = (tier0_progs * PROGRAM_BYTES) as u64;
    tier0[1] = (tier0_pipes * sections[1].len() / pack.pipes.len().max(1)) as u64;
    tier0[6] = tier0_strings as u64;
    tier0[7] = tier0_heap as u64;
    tier0[9] = (pack.splits.iter().filter(|s| (s.whole as usize) < tier0_progs).count() * PACK_SPLIT_BYTES) as u64;
    tier0[10] = tier0_variants as u64;
    tier0[15] = (fast_rows * 16) as u64;

    let header_size = 8 + 16 + 16 * SECTIONS + 16 + 8 * SECTIONS + 16;
    let padded_header = header_size.div_ceil(256) * 256;
    let mut offs = [(0u64, 0u64); SECTIONS];
    let mut body = Vec::new();
    for (i, sec) in sections.iter().enumerate() {
        let align = if i == 7 { TIER_ALIGN } else { 256 };
        let cur = padded_header + body.len();
        let pad = (align - (cur % align)) % align;
        body.resize(body.len() + pad, 0);
        offs[i] = ((padded_header + body.len()) as u64, sec.len() as u64);
        body.extend_from_slice(sec);
    }

    let mut out = Vec::with_capacity(padded_header + body.len());
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&(pack.programs.len() as u32).to_le_bytes());
    out.extend_from_slice(&(pack.pipes.len() as u32).to_le_bytes());
    out.extend_from_slice(&(pack.constexpr_tsc.len() as u32).to_le_bytes());
    for (off, sz) in offs {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&sz.to_le_bytes());
    }
    out.extend_from_slice(&0u64.to_le_bytes());
    out.extend_from_slice(&chip_facts().to_le_bytes());
    out.extend_from_slice(&pack.tier0_slm_per_warp.to_le_bytes());
    for t in tier0 {
        out.extend_from_slice(&t.to_le_bytes());
    }
    out.extend_from_slice(&rules_identity(pack));
    out.resize(padded_header, 0);
    out.extend_from_slice(&body);
    Ok((out, blob))
}

pub fn stamp_pack_bytes(pack_bin: &mut [u8], stack: u64) -> Result<()> {
    if pack_bin.len() < STACK_OFFSET + 8 || &pack_bin[..8] != MAGIC {
        return Err(LinkError::Invalid("not a valid GM20BPK1 pack.bin".into()));
    }
    pack_bin[STACK_OFFSET..STACK_OFFSET + 8].copy_from_slice(&stack.to_le_bytes());
    Ok(())
}

pub fn stamp_pack_file(path: &Path, stack: u64) -> Result<()> {
    let mut bytes = fs::read(path)?;
    stamp_pack_bytes(&mut bytes, stack)?;
    fs::write(path, &bytes)?;
    Ok(())
}

pub fn sections_line(data: &[u8]) -> String {
    let mut parts = Vec::with_capacity(SECTIONS);
    for (i, name) in SECTION_NAMES.iter().enumerate() {
        let size_off = 24 + 16 * i + 8;
        let t0_off = 24 + 16 * SECTIONS + 16 + 8 * i;
        let sz = u64::from_le_bytes(data[size_off..size_off + 8].try_into().expect("8"));
        let t0 = u64::from_le_bytes(data[t0_off..t0_off + 8].try_into().expect("8"));
        parts.push(format!("{name} {} ({})", sz / 1024, t0 / 1024));
    }
    format!("pack sections (KiB, tier 0 KiB): {}", parts.join(", "))
}

pub fn write_layouts(path: &Path, pack: &Pack) -> Result<()> {
    let split_map: HashMap<u32, u32> = pack.splits.iter().map(|s| (s.whole, s.fs)).collect();
    let mut text = String::new();
    for rec in &pack.pipes {
        let slots = rec
            .entries
            .iter()
            .map(|e| {
                let (off, kind) = match *e {
                    PipelineEntry::Ubo { off, .. } | PipelineEntry::ColorTable { off } => (off, 'u'),
                    PipelineEntry::Table { off, kind, .. } => (off, if kind == 10 { 'u' } else { 't' }),
                    PipelineEntry::Handle { off, smp, .. } => (off, if smp.is_some() { 'h' } else { 't' }),
                };
                format!("{off}:{kind}")
            })
            .collect::<Vec<_>>()
            .join(" ");
        let vs = &pack.programs[rec.vs as usize].name;
        for fs_opt in [Some(rec.fs), split_map.get(&rec.fs).copied()] {
            if let Some(fs) = fs_opt
                && fs != NO_PROGRAM
            {
                let fs_name = &pack.programs[fs as usize].name;
                text.push_str(&format!("{vs} {fs_name} {:x} {slots}\n", rec.set_size));
            }
        }
    }
    fs::write(path, text)?;
    Ok(())
}

pub fn write_programs(path: &Path, programs: &[(String, &crate::records::RawProgram)]) -> Result<()> {
    let mut out = Vec::new();
    for (_, p) in programs {
        out.extend_from_slice(b"NVPG");
        out.extend_from_slice(&(p.image.len().saturating_sub(80) as u32).to_le_bytes());
        out.extend_from_slice(&(p.cbufs.len() as u32).to_le_bytes());
        out.extend_from_slice(&(p.data.len() as u32).to_le_bytes());
        out.extend_from_slice(&p.cbufs);
        out.extend_from_slice(&p.image);
        out.extend_from_slice(&p.data);
    }
    fs::write(path, out)?;
    Ok(())
}
