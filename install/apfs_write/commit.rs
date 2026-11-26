// SPDX-License-Identifier: GPL-3.0-or-later
//! Writes a plan in an order that never leaves a mountable half-written container: the old container is invalidated
//! first, and block 0 lands last.

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, BlockTarget};

use crate::WriteError;
use crate::plan::Plan;

const COPY_BLOCKS: u64 = 2048;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Phase {
    Invalidate,
    Copy,
    Metadata,
    Checkpoint,
    BlockZero,
}

pub fn write(src: &dyn BlockSource, target: &mut dyn BlockTarget, plan: &Plan) -> Result<(), WriteError> {
    write_until(src, target, plan, Phase::BlockZero)
}

/// Writes up to and including `last`, for the interruption tests.
pub fn write_until(
    src: &dyn BlockSource,
    target: &mut dyn BlockTarget,
    plan: &Plan,
    last: Phase,
) -> Result<(), WriteError> {
    let zeros = vec![0u8; (plan.desc_area.end - plan.desc_area.start) as usize * BLOCK_SIZE];
    target.write_blocks(0, &zeros[..BLOCK_SIZE])?;
    target.write_blocks(plan.desc_area.start, &zeros)?;
    target.flush()?;
    if last == Phase::Invalidate {
        return Ok(());
    }

    let mut buffer = vec![0u8; COPY_BLOCKS as usize * BLOCK_SIZE];
    for run in plan.copy.set_runs() {
        let mut at = run.start;
        while at < run.end {
            let count = (run.end - at).min(COPY_BLOCKS);
            let bytes = &mut buffer[..count as usize * BLOCK_SIZE];
            src.read_blocks(at, bytes)?;
            target.write_blocks(at, bytes)?;
            at += count;
        }
    }
    for (paddr, bytes) in &plan.data {
        target.write_blocks(*paddr, bytes)?;
    }
    if last == Phase::Copy {
        return target.flush().map_err(Into::into);
    }

    for (paddr, block) in &plan.metadata {
        target.write_blocks(*paddr, &block[..])?;
    }
    target.flush()?;
    if last == Phase::Metadata {
        return Ok(());
    }

    let cp = &plan.checkpoint;
    for (paddr, block) in &cp.ephemeral {
        target.write_blocks(*paddr, &block[..])?;
    }
    target.write_blocks(cp.map.0, &cp.map.1[..])?;
    target.write_blocks(cp.superblock_paddr, &cp.superblock[..])?;
    target.flush()?;
    if last == Phase::Checkpoint {
        return Ok(());
    }

    target.write_blocks(0, &cp.superblock[..])?;
    target.flush()?;
    Ok(())
}
