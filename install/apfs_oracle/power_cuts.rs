// SPDX-License-Identifier: GPL-3.0-or-later
//! Phase-boundary and mid-phase power-cut verification for the full 3-volume pipeline.

use std::fs::{self, File};
use std::io::Read;
use std::path::Path;

use sha2::{Digest, Sha256};
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_read::Container;
use switch_apfs_write::target::{CutTarget, stale_descriptor_area};
use switch_apfs_write::{CheckpointAreas, Phase, write, write_until};

use crate::compose::{ComposeConfig, build_trees, plan_trees};
use crate::{Error, Result};

const PHASE_BOUNDARIES: [(&str, Phase); 5] = [
    ("cut-phase-B.img", Phase::Invalidate),
    ("cut-phase-C.img", Phase::Data),
    ("cut-phase-D.img", Phase::Metadata),
    ("cut-phase-E-before-blk0.img", Phase::Checkpoint),
    ("cut-phase-E-complete.img", Phase::BlockZero),
];

pub fn run(cfg: &ComposeConfig, out_dir: &Path) -> Result<()> {
    fs::create_dir_all(out_dir)?;
    let src = FileSource::open(&cfg.source, cfg.source_offset)?;
    let trees = build_trees(cfg, &src)?;
    let plan = plan_trees(cfg, &src, &trees, &[])?;

    let mut phase_writes = [0u64; 5];
    for (idx, (name, phase)) in PHASE_BOUNDARIES.iter().enumerate() {
        let path = out_dir.join(name);
        let mut target = FileTarget::create(&path, cfg.target_bytes)?;
        let stats = write_until(&src, &mut target, &plan, *phase)?;
        if let Some(last) = stats.last() {
            phase_writes[idx] = last.writes;
        }
        let open_ok = Container::open(&target).is_ok();
        if *phase == Phase::BlockZero {
            if !open_ok {
                return Err(Error::Check(format!("{name}: reader refused complete image")));
            }
        } else if open_ok {
            return Err(Error::Check(format!(
                "{name}: reader opened image before Phase::BlockZero"
            )));
        }
        println!(
            "power-cuts: {name} ({phase:?}) writes={:?} open={open_ok}",
            phase_writes[idx]
        );
    }

    let complete_sha = file_sha256(&out_dir.join("cut-phase-E-complete.img"))?;

    // Mid-phase torn cuts inside C and D must refuse Container::open and resume byte-identically.
    let cuts = [
        ("mid-C", phase_writes[0] + 1),
        ("mid-D", phase_writes[0] + phase_writes[1] + 1),
        (
            "before-blk0",
            phase_writes[0] + phase_writes[1] + phase_writes[2] + phase_writes[3],
        ),
    ];
    for (label, writes_before_cut) in cuts {
        let scratch = out_dir.join(format!("resume-{label}.img"));
        let mut target = FileTarget::create(&scratch, cfg.target_bytes)?;
        let cut_res = write(&src, &mut CutTarget::new(&mut target, writes_before_cut, true), &plan);
        if cut_res.is_ok() {
            return Err(Error::Check(format!("cut {label} unexpectedly succeeded")));
        }
        if Container::open(&target).is_ok() {
            return Err(Error::Check(format!("cut {label} opened before block 0")));
        }
        let fresh_plan = plan_trees(cfg, &src, &trees, &[])?;
        write(&src, &mut target, &fresh_plan)?;
        drop(target);
        let resumed_sha = file_sha256(&scratch)?;
        fs::remove_file(&scratch)?;
        if resumed_sha != complete_sha {
            return Err(Error::Check(format!(
                "cut {label}: resumed image SHA-256 differs from uninterrupted build"
            )));
        }
        println!(
            "power-cuts: torn cut {label} (after {writes_before_cut} writes) refused and resumed byte-identically"
        );
    }

    // Starting from an already-finished container with distinct checkpoint areas: Phase B must invalidate it.
    let mut prior_cfg = cfg.clone();
    prior_cfg.areas = if cfg.areas == CheckpointAreas::MAC_COMPOSED {
        CheckpointAreas::Rule
    } else {
        CheckpointAreas::MAC_COMPOSED
    };
    prior_cfg.time_ns = cfg.time_ns.wrapping_add(1);
    let prior_plan = plan_trees(&prior_cfg, &src, &trees, &[])?;

    let prior_cut_path = out_dir.join("cut-prior-phase-B.img");
    let mut prior_target = FileTarget::create(&prior_cut_path, cfg.target_bytes)?;
    write(&src, &mut prior_target, &prior_plan)?;
    if Container::open(&prior_target).is_err() {
        return Err(Error::Check("prior finished container failed Container::open".into()));
    }
    let stale = stale_descriptor_area(&prior_target)?;
    if stale.is_empty() {
        return Err(Error::Check("stale_descriptor_area missed prior container".into()));
    }
    let replace_plan = plan_trees(cfg, &src, &trees, &stale)?;
    write_until(&src, &mut prior_target, &replace_plan, Phase::Invalidate)?;
    if Container::open(&prior_target).is_ok() {
        return Err(Error::Check("prior container still opened after Phase B".into()));
    }
    println!(
        "power-cuts: cut-prior-phase-B.img invalidated {} stale descriptor range(s)",
        stale.len()
    );
    Ok(())
}

fn file_sha256(path: &Path) -> Result<[u8; 32]> {
    let mut file = File::open(path)?;
    let mut buf = vec![0u8; 4 << 20];
    let mut hasher = Sha256::new();
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hasher.finalize().into())
}
