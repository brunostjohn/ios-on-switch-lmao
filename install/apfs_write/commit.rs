// SPDX-License-Identifier: GPL-3.0-or-later
//! Writes a plan in an order that never leaves a mountable half-written container: the old container is invalidated
//! first, and block 0 lands last. Nothing written is ever read back, and a rerun from phase B writes the same bytes
//! to the same blocks, so it finishes an interrupted run exactly.

use std::time::{Duration, Instant};

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, BlockTarget};

use crate::WriteError;
use crate::content::Window;
use crate::place::NewContent;
use crate::plan::Plan;

const BS: u64 = BLOCK_SIZE as u64;
/// 8 MiB requests: large enough that the target sees sequential writes, small enough to keep two buffers cheap.
const REQUEST_BLOCKS: u64 = 2048;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Phase {
    /// B: block 0 zeroed and flushed, then the descriptor areas zeroed.
    Invalidate,
    /// C: the source's blocks, the copied extents a second time, and new file data.
    Data,
    /// D: every volume's objects, the container object map, the space manager's blocks.
    Metadata,
    /// E, first half: the checkpoint's ephemeral objects, map and superblock.
    Checkpoint,
    /// E, last: block 0.
    BlockZero,
}

pub const PHASES: [Phase; 5] = [
    Phase::Invalidate,
    Phase::Data,
    Phase::Metadata,
    Phase::Checkpoint,
    Phase::BlockZero,
];

#[derive(Clone, Copy, Debug)]
pub struct PhaseStats {
    pub phase: Phase,
    pub writes: u64,
    pub bytes: u64,
    pub elapsed: Duration,
}

impl PhaseStats {
    pub fn mb_per_s(&self) -> f64 {
        self.bytes as f64 / 1e6 / self.elapsed.as_secs_f64().max(1e-9)
    }
}

pub fn write(src: &dyn BlockSource, target: &mut dyn BlockTarget, plan: &Plan) -> Result<Vec<PhaseStats>, WriteError> {
    write_until(src, target, plan, Phase::BlockZero)
}

/// Writes up to and including `last`.
pub fn write_until(
    src: &dyn BlockSource,
    target: &mut dyn BlockTarget,
    plan: &Plan,
    last: Phase,
) -> Result<Vec<PhaseStats>, WriteError> {
    let mut stats = Vec::new();
    let mut out = Counted {
        target,
        writes: 0,
        bytes: 0,
    };
    for phase in PHASES.into_iter().filter(|&p| p <= last) {
        let started = Instant::now();
        let (writes, bytes) = (out.writes, out.bytes);
        match phase {
            Phase::Invalidate => invalidate(&mut out, plan)?,
            Phase::Data => data(src, &mut out, plan)?,
            Phase::Metadata => {
                let mut batch = Batch::new(REQUEST_BLOCKS);
                for (paddr, block) in &plan.metadata {
                    batch.put(&mut out, *paddr, &block[..])?;
                }
                batch.finish(&mut out)?;
            }
            Phase::Checkpoint => {
                let cp = &plan.checkpoint;
                let mut batch = Batch::new(REQUEST_BLOCKS);
                for (paddr, block) in &cp.ephemeral {
                    batch.put(&mut out, *paddr, &block[..])?;
                }
                batch.put(&mut out, cp.map.0, &cp.map.1[..])?;
                batch.put(&mut out, cp.superblock_paddr, &cp.superblock[..])?;
                batch.finish(&mut out)?;
            }
            Phase::BlockZero => out.write_blocks(0, &plan.checkpoint.superblock[..])?,
        }
        out.target.flush()?;
        stats.push(PhaseStats {
            phase,
            writes: out.writes - writes,
            bytes: out.bytes - bytes,
            elapsed: started.elapsed(),
        });
    }
    Ok(stats)
}

fn invalidate(out: &mut Counted, plan: &Plan) -> Result<(), WriteError> {
    let zeros = vec![0u8; REQUEST_BLOCKS as usize * BLOCK_SIZE];
    // Block 0 reaches the medium before any area is zeroed, so no cut leaves an older container's block 0 naming a
    // half-zeroed descriptor area that would mount from an older checkpoint.
    out.write_blocks(0, &zeros[..BLOCK_SIZE])?;
    out.flush()?;
    for range in &plan.invalidate {
        let mut at = range.start.max(1);
        while at < range.end {
            let n = (range.end - at).min(REQUEST_BLOCKS);
            out.write_blocks(at, &zeros[..(n * BS) as usize])?;
            at += n;
        }
    }
    Ok(())
}

/// One pass through the source in block order: each run of kept blocks goes to its own block number, and the part
/// of it a new volume also holds goes again to that volume's blocks from the same buffer. New file data follows.
fn data(src: &dyn BlockSource, out: &mut Counted, plan: &Plan) -> Result<(), WriteError> {
    let mut buffer = vec![0u8; (REQUEST_BLOCKS * BS) as usize];
    let mut moved = Batch::new(REQUEST_BLOCKS);
    let relocations = &plan.relocations;
    let mut next = 0usize;
    for run in plan.copy.set_runs() {
        let mut at = run.start;
        while at < run.end {
            let count = (run.end - at).min(REQUEST_BLOCKS);
            let bytes = &mut buffer[..(count * BS) as usize];
            src.read_blocks(at, bytes)?;
            out.write_blocks(at, bytes)?;
            let end = at + count;
            while next < relocations.len() && relocations[next].src + relocations[next].blocks <= at {
                next += 1;
            }
            let mut r = next;
            while r < relocations.len() && relocations[r].src < end {
                let rel = relocations[r];
                let from = rel.src.max(at);
                let to = (rel.src + rel.blocks).min(end);
                let slice = &bytes[((from - at) * BS) as usize..((to - at) * BS) as usize];
                moved.put(out, rel.dst + (from - rel.src), slice)?;
                r += 1;
            }
            at = end;
        }
    }
    moved.finish(out)?;

    let mut batch = Batch::new(REQUEST_BLOCKS);
    for d in &plan.new_data {
        match &d.content {
            NewContent::Bytes { bytes, offset } => {
                let mut block = 0;
                while block < d.blocks {
                    let n = (d.blocks - block).min(REQUEST_BLOCKS);
                    let start = (offset + (block * BS) as usize).min(bytes.len());
                    let end = (start + (n * BS) as usize).min(bytes.len());
                    let mut chunk = bytes[start..end].to_vec();
                    chunk.resize((n * BS) as usize, 0);
                    batch.put(out, d.dst + block, &chunk)?;
                    block += n;
                }
            }
            NewContent::Fill(fill) => {
                batch.finish(out)?;
                let mut window = Window::new(out, d.dst, d.blocks);
                fill.fill(&mut window)?;
                let unwritten = window.unwritten();
                if unwritten > 0 {
                    return Err(WriteError::Refused(format!(
                        "a fill left {unwritten} of the {} blocks at {} unwritten",
                        d.blocks, d.dst
                    )));
                }
            }
        }
    }
    batch.finish(out)?;
    Ok(())
}

/// Counts what reaches the target.
struct Counted<'a> {
    target: &'a mut dyn BlockTarget,
    writes: u64,
    bytes: u64,
}

impl BlockTarget for Counted<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> std::io::Result<()> {
        self.target.write_blocks(paddr, data)?;
        self.writes += 1;
        self.bytes += data.len() as u64;
        Ok(())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.target.flush()
    }
}

/// Joins writes to consecutive blocks into one request.
struct Batch {
    start: u64,
    buf: Vec<u8>,
    max: usize,
}

impl Batch {
    fn new(blocks: u64) -> Self {
        Self {
            start: 0,
            buf: Vec::new(),
            max: (blocks * BS) as usize,
        }
    }

    fn put(&mut self, out: &mut dyn BlockTarget, paddr: u64, data: &[u8]) -> std::io::Result<()> {
        let end = self.start + (self.buf.len() / BLOCK_SIZE) as u64;
        if !self.buf.is_empty() && (paddr != end || self.buf.len() + data.len() > self.max) {
            self.finish(out)?;
        }
        if self.buf.is_empty() {
            self.start = paddr;
        }
        if data.len() >= self.max {
            return out.write_blocks(paddr, data);
        }
        self.buf.extend_from_slice(data);
        Ok(())
    }

    fn finish(&mut self, out: &mut dyn BlockTarget) -> std::io::Result<()> {
        if !self.buf.is_empty() {
            out.write_blocks(self.start, &self.buf)?;
            self.buf.clear();
        }
        Ok(())
    }
}
