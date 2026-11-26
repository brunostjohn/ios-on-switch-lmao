// SPDX-License-Identifier: GPL-3.0-or-later
//! Drives M1's container layer with a Data volume far bigger than one fs-tree leaf. `switch_apfs_write::plan` lays
//! the container out with an empty Data volume; this then gives that volume multi-level fs, extent-reference and
//! object-map trees, takes their blocks and the files' data from what the plan left free, and rebuilds the space
//! manager and the checkpoint to match. `fsck_apfs` checks that the bitmaps equal exactly the blocks in use.

use std::collections::HashMap;
use std::error::Error;
use std::ops::Range;

use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{BuiltNode, Layout, TreeShape};
use switch_apfs_format::container::NxSuperblock;
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::omap::{OmapKey, OmapVal};
use switch_apfs_format::spaceman::{SFQ_IP, SFQ_MAIN};
use switch_apfs_format::volume::{APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, ApfsSuperblock};
use switch_apfs_format::{BLOCK_SIZE, get, put, seal};
use switch_apfs_read::Container;
use switch_apfs_write::checkpoint::{self, CheckpointInput};
use switch_apfs_write::records::{self, RecordInput};
use switch_apfs_write::volume::{VolumeAddrs, omap_block};
use switch_apfs_write::{Content, Plan, Tree, VolumeSpec, WriteOptions};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U32, U64};

pub struct Placed {
    pub fs: (usize, u16),
    pub extent_ref: (usize, u16),
    pub omap: (usize, u16),
    pub data_blocks: u64,
}

pub fn give_data_its_tree(
    src: &dyn BlockSource,
    plan: &mut Plan,
    spec: &VolumeSpec,
    tree: &Tree,
    options: &WriteOptions,
) -> Result<Placed, Box<dyn Error>> {
    let c = Container::open(src)?;
    let xid = plan.xid;
    let addrs = VolumeAddrs::from_metadata_run(plan.summary.volume_superblock, Vec::new());
    let g = Geometry::for_blocks(plan.target_blocks)?;
    let mut used = used_by(plan, g.blocks);
    if used.count_set(0..g.blocks) != g.blocks - plan.summary.free_blocks {
        return Err("the plan's allocation map can't be rebuilt from the plan".into());
    }

    let numbered = records::number(tree);
    let extents = numbered.extents();
    let data_blocks: u64 = extents.iter().map(|e| e.blocks).sum();
    let mut at = if data_blocks == 0 {
        0
    } else {
        alloc(&mut used, data_blocks)?
    };
    let mut extent_addrs = Vec::new();
    for e in &extents {
        extent_addrs.push(at);
        at += e.blocks;
    }
    let (fs_records, extent_records) = records::build(&RecordInput {
        tree,
        numbered: &numbered,
        extent_addrs: &extent_addrs,
        incompatible_features: APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | spec.incompatible_features,
        unicode: spec.unicode,
        time_ns: options.time_ns,
    })?;

    let mut nx: NxSuperblock = get(&plan.checkpoint.superblock[..], 0).ok_or("short NXSB")?;
    let sb_block = plan
        .metadata
        .iter()
        .find(|(paddr, _)| *paddr == addrs.superblock)
        .map(|(_, b)| b.clone())
        .ok_or("Data's superblock isn't in the plan")?;
    let mut sb: ApfsSuperblock = get(&sb_block[..], 0).ok_or("short volume superblock")?;

    let fs = Layout::new(&TreeShape::FS_TREE, &fs_records)?;
    let fs_addrs = blocks_after(&mut used, addrs.fs_root, fs.node_count())?;
    let root_oid = sb.root_tree_oid.get();
    let mut next_oid = nx.next_oid.get();
    let mut ids = std::iter::once(root_oid).chain(std::iter::from_fn(|| {
        next_oid += 1;
        Some(next_oid - 1)
    }));
    let fs_built = fs.emit(xid, &mut || ids.next().unwrap_or(0));
    nx.next_oid = U64::new(next_oid);

    let extent_ref = Layout::new(&TreeShape::EXTENT_REF, &extent_records)?;
    let extent_ref_addrs = blocks_after(&mut used, addrs.extent_ref, extent_ref.node_count())?;
    let extent_ref_built = emit_physical(&extent_ref, xid, &extent_ref_addrs);

    let mut omap_records: Vec<_> = fs_built
        .nodes
        .iter()
        .zip(&fs_addrs)
        .map(|(node, &paddr)| omap_record(node.id, xid, paddr))
        .collect();
    omap_records.sort_by_key(|(key, _)| u64::from_le_bytes(key[..8].try_into().unwrap_or_default()));
    let omap = Layout::new(&TreeShape::OMAP, &omap_records)?;
    let omap_addrs = blocks_after(&mut used, addrs.omap_tree, omap.node_count())?;
    let omap_built = emit_physical(&omap, xid, &omap_addrs);

    let tree_blocks = (fs_addrs.len() + extent_ref_addrs.len() + omap_addrs.len()) as u64;
    sb.fs_alloc_count = U64::new(tree_blocks + 2 + data_blocks);
    sb.total_blocks_alloced = U64::new(data_blocks);
    sb.next_obj_id = U64::new(numbered.next_inode);
    sb.num_files = U64::new(numbered.files);
    sb.num_directories = U64::new(numbered.dirs);
    let mut sb_block = sb_block;
    put(&mut sb_block[..], 0, &sb);
    seal(&mut sb_block[..]);

    let pool = plan.summary.pool.clone();
    let pool_bitmap = plan.summary.pool_bitmap_ring.start + 1;
    let replaced = [
        addrs.superblock,
        addrs.omap,
        addrs.omap_tree,
        addrs.fs_root,
        addrs.extent_ref,
    ];
    plan.metadata
        .retain(|(p, _)| !replaced.contains(p) && !pool.contains(p) && *p != pool_bitmap);
    plan.metadata.push((addrs.superblock, sb_block));
    plan.metadata
        .push((addrs.omap, omap_block(addrs.omap, addrs.omap_tree, xid, 0)));
    for (node, paddr) in fs_built.nodes.into_iter().zip(&fs_addrs) {
        plan.metadata.push((*paddr, node.block));
    }
    for node in extent_ref_built.into_iter().chain(omap_built) {
        plan.metadata.push((node.id, node.block));
    }

    let sm = &c.space.spaceman;
    let free_queue_oids = [sm.fq[SFQ_IP].tree_oid.get(), sm.fq[SFQ_MAIN].tree_oid.get()];
    let space = space::build(&SpaceInput {
        geometry: &g,
        used: &used,
        pool_base: pool.start,
        ring_base: plan.summary.pool_bitmap_ring.start,
        xid,
        spaceman_oid: c.checkpoint.nx.spaceman_oid.get(),
        free_queue_oids,
    })?;
    plan.metadata.extend(space.blocks);
    plan.summary.free_blocks = space.free_blocks;
    plan.checkpoint = checkpoint::build(CheckpointInput {
        source: &c.checkpoint,
        xid,
        desc: plan.desc_area.clone(),
        data: plan.data_area.clone(),
        spaceman: &space.spaceman,
        free_queue_oids,
        nx,
    })?;

    let contents: HashMap<u64, &Vec<u8>> = numbered
        .nodes
        .iter()
        .filter_map(|n| match &n.entry.content {
            Content::File(bytes) => Some((n.inode, bytes)),
            Content::Dir(_) => None,
        })
        .collect();
    for (e, &paddr) in extents.iter().zip(&extent_addrs) {
        let mut padded = contents[&e.inode].clone();
        padded.resize(e.blocks as usize * BLOCK_SIZE, 0);
        plan.data.push((paddr, padded));
    }
    Ok(Placed {
        fs: (fs.node_count(), fs.height()),
        extent_ref: (extent_ref.node_count(), extent_ref.height()),
        omap: (omap.node_count(), omap.height()),
        data_blocks,
    })
}

/// What `plan` allocated: the source's blocks it keeps, block 0, the checkpoint areas, the pool and its ring, and
/// every block it writes.
fn used_by(plan: &Plan, blocks: u64) -> Bitmap {
    let mut used = Bitmap::new(blocks);
    for run in plan.copy.set_runs() {
        used.set_range(run);
    }
    let ranges: [Range<u64>; 5] = [
        0..1,
        plan.desc_area.clone(),
        plan.data_area.clone(),
        plan.summary.pool.clone(),
        plan.summary.pool_bitmap_ring.clone(),
    ];
    for range in ranges {
        used.set_range(range);
    }
    for (paddr, _) in &plan.metadata {
        used.set_range(*paddr..*paddr + 1);
    }
    for (paddr, bytes) in &plan.data {
        used.set_range(*paddr..*paddr + (bytes.len() / BLOCK_SIZE) as u64);
    }
    used
}

fn alloc(used: &mut Bitmap, count: u64) -> Result<u64, Box<dyn Error>> {
    let at = used.first_clear_run(count).ok_or("the target is full")?;
    used.set_range(at..at + count);
    Ok(at)
}

/// `first`, which the plan already gave the tree's root, then `count - 1` new blocks.
fn blocks_after(used: &mut Bitmap, first: u64, count: usize) -> Result<Vec<u64>, Box<dyn Error>> {
    let rest = count as u64 - 1;
    let at = if rest == 0 { 0 } else { alloc(used, rest)? };
    Ok(std::iter::once(first).chain(at..at + rest).collect())
}

fn emit_physical(layout: &Layout, xid: u64, addrs: &[u64]) -> Vec<BuiltNode> {
    let mut next = addrs.iter().copied();
    layout.emit(xid, &mut || next.next().unwrap_or(0)).nodes
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
