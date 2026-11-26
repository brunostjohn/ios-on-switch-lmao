// SPDX-License-Identifier: GPL-3.0-or-later
//! `power-cuts`: the commit order under a power cut. The write stops after N requests, at every phase boundary and
//! at seeded random points inside each phase, some tearing the request they stop in. Each cut runs onto an empty
//! target and onto one that already holds a finished container with other checkpoint areas.
//!
//! After every cut: our reader must refuse the target, and once phase B is done the only checksummed container
//! superblocks left anywhere are the new checkpoint's own. Then a rerun from phase B, with a plan built afresh, must
//! leave the target equal, block for block, to an uninterrupted run onto the same starting state.

use std::path::{Path, PathBuf};

use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_read::Container;
use switch_apfs_write::commit::PHASES;
use switch_apfs_write::target::{CutTarget, MemTarget, is_nxsb, nxsb_blocks, stale_descriptor_area};
use switch_apfs_write::{CheckpointAreas, Plan, write};

use crate::build::{BuildArgs, Trees, plan_for, trees};
use crate::cut_plan::{Cut, cut_points, ends, lag_block_zero, prior_args, stale_superblock};
use crate::parse::parse_areas;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(clap::Args)]
pub struct CutArgs {
    #[command(flatten)]
    pub build: BuildArgs,
    #[arg(long, default_value_t = 1)]
    pub seed: u64,
    /// Random cuts inside each phase that has room for them.
    #[arg(long, default_value_t = 4)]
    pub per_phase: u64,
    /// Saves the target at each phase boundary of both starting states, for the attach checks.
    #[arg(long)]
    pub save: Option<PathBuf>,
    /// Runs on image files in this directory instead of in memory, for a source too large to hold.
    #[arg(long)]
    pub on_files: Option<PathBuf>,
    /// The finished starting state's checkpoint areas, as `--areas` takes them.
    #[arg(long, value_parser = parse_areas)]
    pub prior_areas: Option<CheckpointAreas>,
    /// The finished starting state's block 0 lags behind its newest checkpoint. In memory only.
    #[arg(long)]
    pub prior_lag: bool,
}

struct State<'a> {
    name: &'static str,
    start: MemTarget,
    plan: Plan,
    expected: MemTarget,
    ends: Vec<u64>,
    stale: Vec<std::ops::Range<u64>>,
    trees: &'a Trees,
}

pub fn run(args: &CutArgs) -> Result<bool> {
    let b = &args.build;
    if let Some(dir) = &args.on_files {
        if args.prior_lag {
            return Err("--prior-lag runs in memory only".into());
        }
        return crate::file_cuts::run(b, dir, args);
    }
    let src = FileSource::open(&b.source, b.source_offset)?;
    let trees = trees(b, &src)?;
    let mut prior = MemTarget::default();
    let prior_plan = plan_for(&prior_args(b, args.prior_areas), &src, &trees, &[])?;
    write(&src, &mut prior, &prior_plan)?;
    if args.prior_lag {
        lag_block_zero(&mut prior, &prior_plan)?;
    }
    let mut states = Vec::new();
    for (name, start) in [("empty", MemTarget::default()), ("finished", prior)] {
        let stale = stale_descriptor_area(&start)?;
        let plan = plan_for(b, &src, &trees, &stale)?;
        let mut expected = start.clone();
        let stats = write(&src, &mut expected, &plan)?;
        states.push(State {
            name,
            start,
            plan,
            expected,
            ends: ends(&stats),
            stale,
            trees: &trees,
        });
    }
    println!("{}", stale_superblock(&prior_plan, &states[1].plan));

    let mut failures = 0;
    let mut count = 0;
    for s in &states {
        let cuts = cut_points(&s.ends, args.seed, args.per_phase);
        let total = s.ends.last().copied().unwrap_or(0);
        println!(
            "{} target: {total} writes uninterrupted; phase ends {:?}",
            s.name, s.ends
        );
        for cut in cuts.iter().filter(|c| c.writes > 0) {
            count += 1;
            let problems = one(b, &src, s, cut)?;
            let label = format!(
                "{} target, cut at write {} of {} ({:?}{}{})",
                s.name,
                cut.writes,
                total,
                PHASES[cut.phase],
                if cut.boundary { ", phase start" } else { "" },
                if cut.torn { ", torn" } else { "" }
            );
            if problems.is_empty() {
                println!("ok: {label}");
            } else {
                failures += 1;
                println!("FAIL: {label}: {}", problems.join("; "));
            }
            if let (Some(dir), true) = (&args.save, cut.boundary) {
                let mut cut_state = s.start.clone();
                let _ = write(&src, &mut CutTarget::new(&mut cut_state, cut.writes, false), &s.plan);
                let path = dir.join(format!("cut-{}-{:?}.img", s.name, PHASES[cut.phase]));
                save(&cut_state, &path, b.target_bytes)?;
            }
        }
        if let (Some(dir), "finished") = (&args.save, s.name) {
            let mut cut_state = s.start.clone();
            let _ = write(&src, &mut CutTarget::new(&mut cut_state, 1, false), &s.plan);
            save(
                &cut_state,
                &dir.join("cut-finished-after-block-zero.img"),
                b.target_bytes,
            )?;
        }
    }
    if let Some(dir) = &args.save {
        save(&states[0].expected, &dir.join("complete.img"), b.target_bytes)?;
    }
    println!("{count} cuts on 2 starting states; failures: {failures}");
    Ok(failures == 0)
}

fn one(b: &BuildArgs, src: &FileSource, s: &State, cut: &Cut) -> Result<Vec<String>> {
    let mut problems = Vec::new();
    let mut mem = s.start.clone();
    if write(src, &mut CutTarget::new(&mut mem, cut.writes, cut.torn), &s.plan).is_ok() {
        problems.push("the cut write finished".into());
    }
    if Container::open(&mem).is_ok() {
        problems.push("our reader opens the target before block 0".into());
    }
    if cut.writes >= s.ends[1] || s.start.blocks.is_empty() {
        let stray: Vec<u64> = nxsb_blocks(&mem)
            .into_iter()
            .filter(|p| !s.plan.desc_area.contains(p) || !s.expected.blocks.get(p).is_some_and(|b| is_nxsb(&b[..])))
            .collect();
        if !stray.is_empty() {
            problems.push(format!("checksummed container superblocks at {stray:?}"));
        }
    }
    write(src, &mut mem, &plan_for(b, src, s.trees, &s.stale)?)?;
    if mem != s.expected {
        let differ = s
            .expected
            .blocks
            .keys()
            .chain(mem.blocks.keys())
            .find(|p| mem.blocks.get(p) != s.expected.blocks.get(p));
        problems.push(format!("the rerun differs, first at block {differ:?}"));
    }
    Ok(problems)
}

fn save(mem: &MemTarget, path: &Path, bytes: u64) -> Result<()> {
    let mut out = FileTarget::create(path, bytes)?;
    for (paddr, block) in &mem.blocks {
        out.write_blocks(*paddr, &block[..])?;
    }
    out.flush()?;
    Ok(())
}
