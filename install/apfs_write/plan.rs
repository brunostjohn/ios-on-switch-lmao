// SPDX-License-Identifier: GPL-3.0-or-later
//! Plans a write into Apple's container: which source blocks stay where they are, what the source's container layer
//! gives back, where every new data block and object goes, and every new metadata block's bytes. Nothing is written
//! here, and no file content is read: the plan holds the content sources, not their bytes.

use std::ops::Range;

use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{TreeShape, leaf_root};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::TYPE_OMAP;
use switch_apfs_format::omap::{OmapKey, OmapPhys};
use switch_apfs_format::spaceman::{SFQ_IP, SFQ_MAIN};
use switch_apfs_format::volume::{APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_SYSTEM};
use switch_apfs_format::{Block, get};
use switch_apfs_read::Container;
use zerocopy::little_endian::{U32, U64};

use crate::WriteError;
use crate::checkpoint::{self, CheckpointInput, Committed};
use crate::place::{self, FreeRuns, NewData, Relocation};
use crate::records::{self, RecordInput};
use crate::spec::{CheckpointAreas, NewVolume, SourcePins, WriteOptions};
use crate::volume::{self, VolumeIds, VolumeInput, VolumeStats, omap_block, omap_record};

pub struct Plan {
    pub target_blocks: u64,
    pub xid: u64,
    pub desc_area: Range<u64>,
    pub data_area: Range<u64>,
    /// Zeroed in phase B once block 0 is: the descriptor area, and the older container's areas `plan` was given.
    pub invalidate: Vec<Range<u64>>,
    /// Source blocks that go to the same block number, verbatim.
    pub copy: Bitmap,
    /// Source blocks that also go to a new volume, in source order.
    pub relocations: Vec<Relocation>,
    /// New file data, in block order.
    pub new_data: Vec<NewData>,
    /// Every new volume's objects, the container object map and the space manager's blocks, in block order.
    pub metadata: Vec<(u64, Box<Block>)>,
    pub checkpoint: Committed,
    pub summary: Summary,
}

#[derive(Debug, Default)]
pub struct Summary {
    pub source_xid: u64,
    pub source_blocks: u64,
    pub queued_freed: u64,
    pub queued_already_free: u64,
    pub container_freed: u64,
    pub copied_blocks: u64,
    pub relocated_blocks: u64,
    pub new_data_blocks: u64,
    pub free_blocks: u64,
    pub pool: Range<u64>,
    pub pool_bitmap_ring: Range<u64>,
    pub volumes: Vec<VolumeSummary>,
}

#[derive(Debug, Default)]
pub struct VolumeSummary {
    pub index: usize,
    pub oid: u64,
    pub stats: VolumeStats,
}

/// `stale` is what `target::stale_descriptor_area` read from the target before the first phase B; every rerun gives
/// the same, so it writes the same blocks.
pub fn plan(
    src: &dyn BlockSource,
    pins: &SourcePins,
    volumes: &[NewVolume],
    options: &WriteOptions,
    stale: &[Range<u64>],
) -> Result<Plan, WriteError> {
    let c = Container::open(src)?;
    let cp = &c.checkpoint;
    let system = check_source(src, &c, pins)?;
    let source_blocks = c.blocks();
    if options.target_blocks < source_blocks {
        return Err(WriteError::Refused(format!(
            "{} blocks is smaller than the source's {source_blocks}",
            options.target_blocks
        )));
    }
    let g = Geometry::for_blocks(options.target_blocks)?;
    let mut summary = Summary {
        source_xid: cp.xid(),
        source_blocks,
        ..Default::default()
    };

    let mut used = Bitmap::new(g.blocks);
    for run in c.space.used.set_runs() {
        used.set_range(run);
    }
    for q in &c.space.queues[SFQ_MAIN] {
        let range = q.paddr..q.paddr + q.len;
        if range.end > source_blocks || range.start == 0 {
            return Err(WriteError::Source("a main free-queue entry outside the container"));
        }
        let set = used.count_set(range.clone());
        summary.queued_freed += set;
        summary.queued_already_free += q.len - set;
        used.clear_range(range);
    }
    for q in &c.space.queues[SFQ_IP] {
        if q.paddr < c.space.pool.start || q.paddr + q.len > c.space.pool.end {
            return Err(WriteError::Source("an internal-pool free-queue entry outside the pool"));
        }
    }
    let before = used.count_set(0..g.blocks);
    used.clear_range(c.space.pool.clone());
    used.clear_range(c.space.pool_bitmap_ring.clone());
    for &paddr in std::iter::once(&c.omap.paddr).chain(&c.omap.nodes) {
        used.clear_range(paddr..paddr + 1);
    }
    let areas = match options.areas {
        CheckpointAreas::Rule => Some((g.checkpoint.desc_blocks, g.checkpoint.data_blocks)),
        CheckpointAreas::Sized { desc, data } => Some((desc, data)),
        CheckpointAreas::Keep => None,
    };
    if areas.is_some() {
        used.clear_range(cp.desc_area.clone());
        used.clear_range(cp.data_area.clone());
    }
    summary.container_freed = before - used.count_set(0..g.blocks);

    let mut copy = used.clone();
    copy.clear_range(0..1);
    copy.clear_range(cp.desc_area.clone());
    copy.clear_range(cp.data_area.clone());
    summary.copied_blocks = copy.count_set(0..g.blocks);
    if !used.get(0) {
        return Err(WriteError::Source("block 0 isn't marked in use"));
    }

    let mut alloc = |count: u64| -> Result<u64, WriteError> {
        let at = used.first_clear_run(count).ok_or(WriteError::Full(count))?;
        used.set_range(at..at + count);
        Ok(at)
    };
    let (desc_area, data_area) = match areas {
        None => (cp.desc_area.clone(), cp.data_area.clone()),
        Some((desc, data)) => {
            let data_base = alloc(u64::from(data))?;
            let desc_base = alloc(u64::from(desc))?;
            (
                desc_base..desc_base + u64::from(desc),
                data_base..data_base + u64::from(data),
            )
        }
    };
    let ring = alloc(POOL_BITMAP_BLOCKS)?;
    let pool = alloc(g.pool_blocks)?;
    let omap_obj = alloc(2)?;

    let mut free = FreeRuns::from_bitmap(&used);
    let trees: Vec<_> = volumes.iter().map(|v| v.tree).collect();
    let placement = place::place(&trees, &mut free)?;
    for r in &placement.relocations {
        if !copy.all_set(r.src..r.src + r.blocks) {
            return Err(WriteError::Source(
                "a copied extent outside the blocks the source keeps",
            ));
        }
    }
    let mut taken = placement.taken;
    summary.relocated_blocks = placement.relocations.iter().map(|r| r.blocks).sum();
    summary.new_data_blocks = placement.new_data.iter().map(|d| d.blocks).sum();

    let xid = cp.nx.next_xid.get().max(cp.xid() + 1);
    let mut nx = cp.nx;
    let mut next_oid = nx.next_oid.get();
    let mut metadata = Vec::new();
    let mut omap_records = Vec::new();
    for v in &c.volumes {
        omap_records.push(omap_record(v.oid, v.xid, v.paddr));
    }
    for (n, v) in volumes.iter().enumerate() {
        let slot = nx
            .fs_oid
            .iter()
            .position(|o| o.get() == 0)
            .filter(|&s| s < g.max_file_systems as usize)
            .ok_or_else(|| WriteError::Refused("no free volume slot".into()))?;
        let ids = VolumeIds {
            fs_index: slot as u32,
            superblock_oid: next_oid,
            xid,
        };
        next_oid += 1;
        nx.fs_oid[slot] = U64::new(ids.superblock_oid);
        let records = records::build(&RecordInput {
            tree: v.tree,
            extents: &placement.extents[n],
            incompatible_features: APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | v.spec.incompatible_features,
            readonly_compatible_features: v.spec.readonly_compatible_features,
            unicode: v.spec.unicode,
            time_ns: options.time_ns,
        })?;
        let input = VolumeInput {
            spec: v.spec,
            records: &records,
            ids: &ids,
            options,
            system: &system[..],
        };
        let built = volume::build(&input, &mut next_oid, &mut free, &mut taken)?;
        omap_records.push(omap_record(ids.superblock_oid, xid, built.stats.superblock));
        metadata.extend(built.blocks);
        summary.volumes.push(VolumeSummary {
            index: slot,
            oid: ids.superblock_oid,
            stats: built.stats,
        });
    }
    for run in taken {
        used.set_range(run);
    }
    nx.next_oid = U64::new(next_oid);
    nx.block_count = U64::new(g.blocks);
    nx.max_file_systems = U32::new(g.max_file_systems);
    nx.omap_oid = U64::new(omap_obj);

    omap_records.sort_by_key(|(k, _)| {
        let k: OmapKey = get(k, 0).unwrap_or_default();
        (k.oid.get(), k.xid.get())
    });
    metadata.push((
        omap_obj + 1,
        leaf_root(&TreeShape::OMAP, omap_obj + 1, xid, &omap_records)?,
    ));
    metadata.push((
        omap_obj,
        omap_block(omap_obj, omap_obj + 1, xid, c.omap.phys.flags.get()),
    ));

    let sm = &c.space.spaceman;
    let free_queue_oids = [sm.fq[SFQ_IP].tree_oid.get(), sm.fq[SFQ_MAIN].tree_oid.get()];
    if free_queue_oids.contains(&0) {
        return Err(WriteError::Refused("a free queue without a tree".into()));
    }
    let space = space::build(&SpaceInput {
        geometry: &g,
        used: &used,
        pool_base: pool,
        ring_base: ring,
        xid,
        spaceman_oid: cp.nx.spaceman_oid.get(),
        free_queue_oids,
    })?;
    let checkpoint = checkpoint::build(CheckpointInput {
        source: cp,
        xid,
        desc: desc_area.clone(),
        data: data_area.clone(),
        spaceman: &space.spaceman,
        free_queue_oids,
        nx,
    })?;
    metadata.extend(space.blocks);
    metadata.sort_by_key(|(paddr, _)| *paddr);
    if metadata.windows(2).any(|w| w[0].0 == w[1].0) {
        return Err(WriteError::Spec("two metadata objects planned at one block".into()));
    }

    summary.free_blocks = space.free_blocks;
    summary.pool = pool..pool + g.pool_blocks;
    summary.pool_bitmap_ring = ring..ring + POOL_BITMAP_BLOCKS;
    let invalidate = std::iter::once(desc_area.clone())
        .chain(
            stale
                .iter()
                .map(|r| r.start.min(g.blocks)..r.end.min(g.blocks))
                .filter(|r| !r.is_empty()),
        )
        .collect();
    Ok(Plan {
        target_blocks: g.blocks,
        xid,
        invalidate,
        desc_area,
        data_area,
        copy,
        relocations: placement.relocations,
        new_data: placement.new_data,
        metadata,
        checkpoint,
        summary,
    })
}

/// The one volume Apple's OS image holds: System, unsealed, with no snapshots. Returns its superblock block.
fn check_source(src: &dyn BlockSource, c: &Container, pins: &SourcePins) -> Result<Box<Block>, WriteError> {
    let [system] = c.volumes.as_slice() else {
        return Err(WriteError::Refused(format!(
            "{} volumes in the source, expected only System",
            c.volumes.len()
        )));
    };
    let sb = &system.sb;
    let refuse = |what: String| Err(WriteError::Refused(what));
    if sb.role.get() != APFS_VOL_ROLE_SYSTEM {
        return refuse(format!("the source volume's role is {:#x}", sb.role.get()));
    }
    if sb.vol_uuid != pins.system_uuid {
        return refuse("the source volume isn't the pinned System volume".into());
    }
    if sb.incompatible_features.get() != APFS_INCOMPAT_NORMALIZATION_INSENSITIVE {
        return refuse(format!(
            "System's incompatible features are {:#x}",
            sb.incompatible_features.get()
        ));
    }
    let omap_block = src.block(sb.omap_oid.get())?;
    let omap: OmapPhys = get(&omap_block[..], 0).ok_or(WriteError::Source("System's object map"))?;
    if sb.num_snapshots.get() != 0 || omap.snap_count.get() != 0 || c.omap.phys.snap_count.get() != 0 {
        return refuse("the source has snapshots".into());
    }
    if omap.o.object_type() != TYPE_OMAP {
        return Err(WriteError::Source("System's object map"));
    }
    Ok(system.block.clone())
}
