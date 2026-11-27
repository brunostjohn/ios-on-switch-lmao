// SPDX-License-Identifier: GPL-3.0-or-later
//! The power-cut suite on image files, for a real source too large to hold in memory. Each cut writes a fresh
//! sparse image, or a clone of a finished container with other checkpoint areas, until the cut. It checks that our
//! reader refuses the image and that every checksummed container superblock written lies in the new descriptor area.
//! Then it reruns from phase B with a plan built afresh and compares the image byte for byte with an uninterrupted run
//! onto the same starting state.
//!
//! Left on disk for the attach checks: each starting state cut just before block 0 lands, and the finished one right
//! after phase B has zeroed its block 0.

use std::fs::{self, File};
use std::io::{self, Read};
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_read::Container;
use switch_apfs_write::commit::PHASES;
use switch_apfs_write::target::{CutTarget, is_nxsb, stale_descriptor_area};
use switch_apfs_write::{Plan, write};

use crate::build::{BuildArgs, Trees, plan_for, trees};
use crate::cut_plan::{Cut, cut_points, ends, prior_args, stale_superblock};
use crate::cuts::CutArgs;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Notes every block written that holds a checksummed container superblock.
struct Watch<'a> {
    inner: &'a mut dyn BlockTarget,
    nxsbs: Vec<u64>,
}

impl BlockTarget for Watch<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        for (i, block) in data.chunks(BLOCK_SIZE).enumerate() {
            if is_nxsb(block) {
                self.nxsbs.push(paddr + i as u64);
            }
        }
        self.inner.write_blocks(paddr, data)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

struct State {
    name: &'static str,
    start: Option<PathBuf>,
    plan: Plan,
    expected: PathBuf,
    ends: Vec<u64>,
    stale: Vec<Range<u64>>,
}

fn clone_or_create(start: Option<&Path>, path: &Path, bytes: u64) -> Result<FileTarget> {
    if path.exists() {
        fs::remove_file(path)?;
    }
    match start {
        Some(from) => {
            if !Command::new("/bin/cp")
                .arg("-c")
                .arg(from)
                .arg(path)
                .status()?
                .success()
            {
                return Err(format!("cp -c {} failed", from.display()).into());
            }
            Ok(FileTarget::open(path)?)
        }
        None => Ok(FileTarget::create(path, bytes)?),
    }
}

/// The first block at which two files differ.
fn first_difference(a: &Path, b: &Path) -> Result<Option<u64>> {
    let (mut fa, mut fb) = (File::open(a)?, File::open(b)?);
    let (mut ba, mut bb) = (vec![0u8; 8 << 20], vec![0u8; 8 << 20]);
    let mut at = 0u64;
    loop {
        let n = fa.read(&mut ba)?;
        fb.read_exact(&mut bb[..n])?;
        if n == 0 {
            return Ok(None);
        }
        if ba[..n] != bb[..n] {
            let i = ba[..n].iter().zip(&bb[..n]).position(|(x, y)| x != y).unwrap_or(0) as u64;
            return Ok(Some((at + i) / BLOCK_SIZE as u64));
        }
        at += n as u64;
    }
}

pub fn run(b: &BuildArgs, dir: &Path, args: &CutArgs) -> Result<bool> {
    let started = Instant::now();
    let src = FileSource::open(&b.source, b.source_offset)?;
    let trees = trees(b, &src)?;
    let finished = dir.join("finished.img");
    let prior_plan = plan_for(&prior_args(b, args.prior_areas), &src, &trees, &[])?;
    write(
        &src,
        &mut clone_or_create(None, &finished, b.target_bytes)?,
        &prior_plan,
    )?;
    let mut states = Vec::new();
    for (name, start) in [("empty", None), ("finished", Some(finished.clone()))] {
        let stale = match &start {
            Some(path) => stale_descriptor_area(&FileSource::open(path, 0)?)?,
            None => Vec::new(),
        };
        let plan = plan_for(b, &src, &trees, &stale)?;
        let expected = dir.join(format!("expected-{name}.img"));
        let t = Instant::now();
        let stats = write(
            &src,
            &mut clone_or_create(start.as_deref(), &expected, b.target_bytes)?,
            &plan,
        )?;
        let ends = ends(&stats);
        println!(
            "{name} target: uninterrupted in {:.1?}, phase ends {ends:?}",
            t.elapsed()
        );
        states.push(State {
            name,
            start,
            plan,
            expected,
            ends,
            stale,
        });
    }
    println!("{}", stale_superblock(&prior_plan, &states[1].plan));

    let mut failures = 0;
    let mut count = 0;
    let cut_path = dir.join("cut.img");
    for s in &states {
        let total = s.ends.last().copied().unwrap_or(0);
        for cut in cut_points(&s.ends, args.seed, args.per_phase)
            .iter()
            .filter(|c| c.writes > 0)
        {
            count += 1;
            let t = Instant::now();
            let problems = one(b, &src, &trees, s, &cut_path, cut)?;
            let label = format!(
                "{} target, cut at write {} of {total} ({:?}{}{}) in {:.1?}",
                s.name,
                cut.writes,
                PHASES[cut.phase],
                if cut.boundary { ", phase start" } else { "" },
                if cut.torn { ", torn" } else { "" },
                t.elapsed()
            );
            if problems.is_empty() {
                println!("ok: {label}");
            } else {
                failures += 1;
                println!("FAIL: {label}: {}", problems.join("; "));
            }
        }
        let keep = dir.join(format!("{}-before-block-zero.img", s.name));
        let mut target = clone_or_create(s.start.as_deref(), &keep, b.target_bytes)?;
        let _ = write(&src, &mut CutTarget::new(&mut target, s.ends[4], false), &s.plan);
        fs::remove_file(&s.expected)?;
    }
    let keep = dir.join("finished-after-block-zero.img");
    let _ = write(
        &src,
        &mut CutTarget::new(&mut clone_or_create(Some(&finished), &keep, 0)?, 1, false),
        &states[1].plan,
    );
    for p in [&cut_path, &finished] {
        let _ = fs::remove_file(p);
    }
    println!(
        "{count} cuts on 2 starting states in {:.1?}; failures: {failures}",
        started.elapsed()
    );
    Ok(failures == 0)
}

fn one(b: &BuildArgs, src: &FileSource, trees: &Trees, s: &State, path: &Path, cut: &Cut) -> Result<Vec<String>> {
    let mut problems = Vec::new();
    let mut file = clone_or_create(s.start.as_deref(), path, b.target_bytes)?;
    let mut watch = Watch {
        inner: &mut file,
        nxsbs: Vec::new(),
    };
    if write(src, &mut CutTarget::new(&mut watch, cut.writes, cut.torn), &s.plan).is_ok() {
        problems.push("the cut write finished".into());
    }
    let stray: Vec<u64> = watch
        .nxsbs
        .iter()
        .copied()
        .filter(|p| !s.plan.desc_area.contains(p))
        .collect();
    if Container::open(&FileSource::open(path, 0)?).is_ok() {
        problems.push("our reader opens the target before block 0".into());
    }
    if !stray.is_empty() {
        problems.push(format!("checksummed container superblocks written at {stray:?}"));
    }
    write(src, &mut FileTarget::open(path)?, &plan_for(b, src, trees, &s.stale)?)?;
    if let Some(block) = first_difference(&s.expected, path)? {
        problems.push(format!("the rerun differs, first at block {block}"));
    }
    Ok(problems)
}
