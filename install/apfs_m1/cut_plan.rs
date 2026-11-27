// SPDX-License-Identifier: GPL-3.0-or-later
//! What both power-cut suites share: the second starting state's build, where each plan's phases end, the cut
//! points, and whether that state's own checkpoint is cleared by phase B alone or also by the new layout.

use std::ops::Range;

use switch_apfs_format::container::NxSuperblock;
use switch_apfs_format::io::BlockTarget;
use switch_apfs_format::{get, put, seal};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::{CheckpointAreas, PhaseStats, Plan};

use crate::build::BuildArgs;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

pub struct Cut {
    pub writes: u64,
    pub torn: bool,
    pub phase: usize,
    pub boundary: bool,
}

/// The write count at each phase's end, from 0.
pub fn ends(stats: &[PhaseStats]) -> Vec<u64> {
    let mut ends = vec![0u64];
    for s in stats {
        ends.push(ends.last().copied().unwrap_or(0) + s.writes);
    }
    ends
}

/// The finished container the second starting state holds: the same volumes at another time, with `areas` or, by
/// default, checkpoint areas sized unlike the build's own.
pub fn prior_args(b: &BuildArgs, areas: Option<CheckpointAreas>) -> BuildArgs {
    let mut other = b.clone();
    other.areas = areas.unwrap_or(if b.areas == CheckpointAreas::MAC_COMPOSED {
        CheckpointAreas::Rule
    } else {
        CheckpointAreas::MAC_COMPOSED
    });
    other.time_ns += 1;
    other
}

/// Makes the finished container's block 0 lag behind its checkpoints, a made-up layout for the lag G0 saw: block 0
/// names an older 8-block descriptor area at the target's end, whose newest superblock names the live area.
pub fn lag_block_zero(prior: &mut MemTarget, plan: &Plan) -> Result<()> {
    let older = plan.target_blocks - 8..plan.target_blocks;
    if prior.blocks.range(older.clone()).next().is_some() {
        return Err("the finished container uses the target's last 8 blocks".into());
    }
    let live: NxSuperblock = get(&plan.checkpoint.superblock[..], 0).ok_or("the finished container's superblock")?;
    let xid = live.o.xid.get();
    let superblock = |xid: u64, desc: Option<&Range<u64>>| {
        let mut nx = live;
        nx.o.xid = xid.into();
        if let Some(desc) = desc {
            nx.xp_desc_base = desc.start.into();
            nx.xp_desc_blocks = ((desc.end - desc.start) as u32).into();
        }
        let mut block = plan.checkpoint.superblock.clone();
        put(&mut block[..], 0, &nx);
        seal(&mut block[..]);
        block
    };
    let first = superblock(xid - 2, Some(&older));
    prior.write_blocks(older.start, &first[..])?;
    prior.write_blocks(older.start + 1, &superblock(xid - 1, None)[..])?;
    prior.write_blocks(0, &first[..])?;
    Ok(())
}

/// Each phase's first write, and `per_phase` seeded points inside every phase with room, half of them torn.
pub fn cut_points(ends: &[u64], seed: u64, per_phase: u64) -> Vec<Cut> {
    let mut state = seed;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let mut cuts = Vec::new();
    for phase in 0..ends.len() - 1 {
        let (first, end) = (ends[phase], ends[phase + 1]);
        cuts.push(Cut {
            writes: first,
            torn: false,
            phase,
            boundary: true,
        });
        if end - first < 2 {
            continue;
        }
        for k in 0..per_phase {
            cuts.push(Cut {
                writes: first + 1 + next() % (end - first - 1),
                torn: k % 2 == 1,
                phase,
                boundary: false,
            });
        }
    }
    cuts
}

/// Whether phases C, D or E write `block`.
fn later_phases_write(plan: &Plan, block: u64) -> bool {
    let cp = &plan.checkpoint;
    block == 0
        || plan.copy.get(block)
        || plan
            .relocations
            .iter()
            .any(|r| (r.dst..r.dst + r.blocks).contains(&block))
        || plan.new_data.iter().any(|d| (d.dst..d.dst + d.blocks).contains(&block))
        || plan.metadata.binary_search_by_key(&block, |(p, _)| *p).is_ok()
        || cp.ephemeral.iter().any(|(p, _)| *p == block)
        || cp.map.0 == block
        || cp.superblock_paddr == block
}

/// What clears the second starting state's own container superblock, which `prior` wrote: phase B's invalidation of
/// its descriptor area is under test only when nothing else would.
pub fn stale_superblock(prior: &Plan, plan: &Plan) -> String {
    let at = prior.checkpoint.superblock_paddr;
    let by = if plan.desc_area.contains(&at) {
        "the new descriptor area's zeroing as well"
    } else if later_phases_write(plan, at) {
        "the new layout's writes as well"
    } else {
        "only the invalidation of its descriptor area"
    };
    format!("the finished container's superblock at block {at} is cleared by {by}")
}
