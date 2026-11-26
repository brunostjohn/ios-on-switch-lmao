// SPDX-License-Identifier: GPL-3.0-or-later
//! Plans a write into Apple's container: which source blocks stay where they are, what the source's container layer
//! gives back, and where every new object goes. Nothing is written here.

use std::ops::Range;

use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{TreeShape, leaf_root};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::TYPE_OMAP;
use switch_apfs_format::omap::{OmapKey, OmapPhys, OmapVal};
use switch_apfs_format::spaceman::{SFQ_IP, SFQ_MAIN};
use switch_apfs_format::volume::{APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_SYSTEM};
use switch_apfs_format::{BLOCK_SIZE, Block, get};
use switch_apfs_read::Container;
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U32, U64};

use crate::WriteError;
use crate::checkpoint::{self, CheckpointInput, Committed};
use crate::records;
use crate::spec::{CheckpointAreas, SourcePins, Tree, VolumeSpec, WriteOptions};
use crate::volume::{self, METADATA_BLOCKS, VolumeAddrs, VolumeIds};

pub struct Plan {
    pub target_blocks: u64,
    pub xid: u64,
    pub desc_area: Range<u64>,
    pub data_area: Range<u64>,
    /// Source blocks that go to the same block number, verbatim.
    pub copy: Bitmap,
    /// New file data, by first block.
    pub data: Vec<(u64, Vec<u8>)>,
    /// New metadata: the volume's objects, the container object map and the space manager's blocks.
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
    pub free_blocks: u64,
    pub volume_oid: u64,
    pub volume_superblock: u64,
    pub volume_index: usize,
    pub pool: Range<u64>,
    pub pool_bitmap_ring: Range<u64>,
}

pub fn plan(
    src: &dyn BlockSource,
    pins: &SourcePins,
    spec: &VolumeSpec,
    tree: &Tree,
    options: &WriteOptions,
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
    if let CheckpointAreas::Sized { .. } = options.areas {
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
    let (desc_area, data_area) = match options.areas {
        CheckpointAreas::Keep => (cp.desc_area.clone(), cp.data_area.clone()),
        CheckpointAreas::Sized { desc, data } => {
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
    let numbered = records::number(tree);
    let volume_first = alloc(METADATA_BLOCKS)?;
    let mut extents = Vec::new();
    for extent in numbered.extents() {
        extents.push(alloc(extent.blocks)?);
    }
    let addrs = VolumeAddrs::from_metadata_run(volume_first, extents);

    let xid = cp.nx.next_xid.get().max(cp.xid() + 1);
    let mut nx = cp.nx;
    let volume_oid = nx.next_oid.get();
    let root_tree_oid = volume_oid + 1;
    nx.next_oid = U64::new(volume_oid + 2);
    let slot = nx
        .fs_oid
        .iter()
        .position(|o| o.get() == 0)
        .filter(|&s| s < g.max_file_systems as usize);
    let slot = slot.ok_or_else(|| WriteError::Refused("no free volume slot".into()))?;
    nx.fs_oid[slot] = U64::new(volume_oid);
    nx.block_count = U64::new(g.blocks);
    nx.max_file_systems = U32::new(g.max_file_systems);
    nx.omap_oid = U64::new(omap_obj);

    let ids = VolumeIds {
        fs_index: slot as u32,
        superblock_oid: volume_oid,
        root_tree_oid,
        xid,
    };
    let built = volume::build(spec, tree, &numbered, &ids, &addrs, options, &system[..])?;

    let mut omap_records = Vec::new();
    for v in &c.volumes {
        omap_records.push(omap_record(v.oid, v.xid, v.paddr));
    }
    omap_records.push(omap_record(volume_oid, xid, addrs.superblock));
    omap_records.sort_by(|a, b| key_of(&a.0).cmp(&key_of(&b.0)));
    let omap_tree = leaf_root(&TreeShape::OMAP, omap_obj + 1, xid, &omap_records)?;
    let omap = volume::omap_block(omap_obj, omap_obj + 1, xid, c.omap.phys.flags.get());

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

    let mut metadata = built.blocks;
    metadata.push((omap_obj, omap));
    metadata.push((omap_obj + 1, omap_tree));
    metadata.extend(space.blocks);
    summary.free_blocks = space.free_blocks;
    summary.volume_oid = volume_oid;
    summary.volume_superblock = addrs.superblock;
    summary.volume_index = slot;
    summary.pool = pool..pool + g.pool_blocks;
    summary.pool_bitmap_ring = ring..ring + POOL_BITMAP_BLOCKS;
    Ok(Plan {
        target_blocks: g.blocks,
        xid,
        desc_area,
        data_area,
        copy,
        data: built.data,
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

fn omap_record(oid: u64, xid: u64, paddr: u64) -> (Vec<u8>, Option<Vec<u8>>) {
    let key = OmapKey {
        oid: U64::new(oid),
        xid: U64::new(xid),
    };
    let value = OmapVal {
        flags: U32::ZERO,
        size: U32::new(BLOCK_SIZE as u32),
        paddr: U64::new(paddr),
    };
    (key.as_bytes().to_vec(), Some(value.as_bytes().to_vec()))
}

fn key_of(key: &[u8]) -> (u64, u64) {
    let k: OmapKey = get(key, 0).unwrap_or_default();
    (k.oid.get(), k.xid.get())
}
