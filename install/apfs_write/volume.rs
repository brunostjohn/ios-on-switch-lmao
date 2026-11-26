// SPDX-License-Identifier: GPL-3.0-or-later
//! A new volume's own objects: superblock, object map, fs tree, extent-reference tree and snapshot-metadata tree, at
//! blocks taken from the free runs. The trees are as deep as their records need.

use std::ops::Range;

use switch_apfs_btree::{Layout, TreeShape, leaf_root};
use switch_apfs_format::fs::MIN_USER_INO_NUM;
use switch_apfs_format::object::{OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BTREE, TYPE_FS, TYPE_OMAP};
use switch_apfs_format::omap::{OmapKey, OmapPhys, OmapVal};
use switch_apfs_format::volume::{
    APFS_FS_UNENCRYPTED, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_MAGIC, APFS_MODIFIED_NAMELEN,
    APFS_SUPERBLOCK_END, APFS_VOLNAME_LEN, ApfsSuperblock, ModifiedBy,
};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal};
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromZeros, IntoBytes};

use crate::WriteError;
use crate::place::FreeRuns;
use crate::records::{Record, Records};
use crate::spec::{VolumeSpec, WriteOptions};

/// The fewest `next_doc_id` macOS leaves on a new volume.
const FIRST_DOC_ID: u32 = 3;

pub struct VolumeIds {
    pub fs_index: u32,
    pub superblock_oid: u64,
    pub xid: u64,
}

#[derive(Clone, Debug, Default)]
pub struct VolumeStats {
    pub superblock: u64,
    pub fs_nodes: usize,
    pub fs_levels: u16,
    pub extent_ref_nodes: usize,
    pub omap_nodes: usize,
    pub metadata_blocks: u64,
    pub data_blocks: u64,
    pub shared_blocks: u64,
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub clone_mappings: u64,
}

pub struct BuiltVolume {
    pub blocks: Vec<(u64, Box<Block>)>,
    pub stats: VolumeStats,
}

pub struct VolumeInput<'a> {
    pub spec: &'a VolumeSpec,
    pub records: &'a Records,
    pub ids: &'a VolumeIds,
    pub options: &'a WriteOptions,
    /// The source's System superblock block, whose words past the Reference's struct a new volume copies.
    pub system: &'a [u8],
}

/// `next_oid` is the container's; the fs-tree nodes take their oids from it.
pub fn build(
    input: &VolumeInput,
    next_oid: &mut u64,
    free: &mut FreeRuns,
    taken: &mut Vec<Range<u64>>,
) -> Result<BuiltVolume, WriteError> {
    let xid = input.ids.xid;
    let records = input.records;
    let fs = Layout::new(&TreeShape::FS_TREE, &records.fs)?;
    let extent_ref = Layout::new(&TreeShape::EXTENT_REF, &records.extent_refs)?;
    let fs_oids: Vec<u64> = (*next_oid..*next_oid + fs.node_count() as u64).collect();
    *next_oid += fs.node_count() as u64;
    let placeholder: Vec<Record> = fs_oids.iter().map(|&oid| omap_record(oid, xid, 0)).collect();
    let omap_count = Layout::new(&TreeShape::OMAP, &placeholder)?.node_count();

    let total = 3 + omap_count + fs.node_count() + extent_ref.node_count();
    let mut addrs = Vec::with_capacity(total);
    for run in free.take(total as u64)? {
        addrs.extend(run.clone());
        taken.push(run);
    }
    let (superblock_at, omap_at, snap_at) = (addrs[0], addrs[1], addrs[2]);
    let (omap_addrs, rest) = addrs[3..].split_at(omap_count);
    let (fs_addrs, extent_ref_addrs) = rest.split_at(fs.node_count());

    let mut blocks = Vec::with_capacity(total);
    let mut oids = fs_oids.iter().copied();
    let fs_built = fs.emit(xid, &mut || oids.next().unwrap_or(0));
    blocks.extend(fs_built.nodes.into_iter().zip(fs_addrs).map(|(n, &p)| (p, n.block)));
    let omap_records: Vec<Record> = fs_oids
        .iter()
        .zip(fs_addrs)
        .map(|(&oid, &paddr)| omap_record(oid, xid, paddr))
        .collect();
    let omap = Layout::new(&TreeShape::OMAP, &omap_records)?;
    blocks.extend(physical(&omap, xid, omap_addrs));
    blocks.extend(physical(&extent_ref, xid, extent_ref_addrs));
    blocks.push((snap_at, leaf_root(&TreeShape::SNAP_META, snap_at, xid, &[])?));
    blocks.push((omap_at, omap_block(omap_at, omap_addrs[0], xid, 0)));

    let stats = VolumeStats {
        superblock: superblock_at,
        fs_nodes: fs.node_count(),
        fs_levels: fs.height(),
        extent_ref_nodes: extent_ref.node_count(),
        omap_nodes: omap_count,
        metadata_blocks: total as u64,
        data_blocks: records.data_blocks,
        shared_blocks: records.shared_blocks,
        files: records.files,
        dirs: records.dirs,
        symlinks: records.symlinks,
        clone_mappings: records.clone_mappings,
    };
    let objects = Objects {
        omap: omap_at,
        root_tree_oid: fs_oids[0],
        extent_ref: extent_ref_addrs[0],
        snap_meta: snap_at,
    };
    blocks.push((superblock_at, superblock(input, &objects, &stats)?));
    Ok(BuiltVolume { blocks, stats })
}

fn physical(layout: &Layout, xid: u64, addrs: &[u64]) -> Vec<(u64, Box<Block>)> {
    let mut next = addrs.iter().copied();
    let built = layout.emit(xid, &mut || next.next().unwrap_or(0));
    built.nodes.into_iter().map(|n| (n.id, n.block)).collect()
}

pub fn omap_record(oid: u64, xid: u64, paddr: u64) -> Record {
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

pub fn omap_block(paddr: u64, tree: u64, xid: u64, flags: u32) -> Box<Block> {
    let phys = OmapPhys {
        o: ObjPhys::new(paddr, xid, OBJ_PHYSICAL | TYPE_OMAP, 0),
        flags: U32::new(flags),
        snap_count: U32::ZERO,
        tree_type: U32::new(OBJ_PHYSICAL | TYPE_BTREE),
        snapshot_tree_type: U32::new(OBJ_PHYSICAL | TYPE_BTREE),
        tree_oid: U64::new(tree),
        ..Default::default()
    };
    let mut block = Box::new([0u8; BLOCK_SIZE]);
    put(&mut block[..], 0, &phys);
    seal(&mut block[..]);
    block
}

struct Objects {
    omap: u64,
    root_tree_oid: u64,
    extent_ref: u64,
    snap_meta: u64,
}

/// The words Apple's kext keeps past the Reference's struct are taken from the System volume of the same IPSW, as
/// if that kext had formatted this volume too; the first is the superblock's own xid. `fs_alloc_count` leaves the
/// superblock out, as Apple's own count does.
fn superblock(input: &VolumeInput, objects: &Objects, stats: &VolumeStats) -> Result<Box<Block>, WriteError> {
    let (spec, options, records) = (input.spec, input.options, input.records);
    let system_sb: ApfsSuperblock =
        switch_apfs_format::get(input.system, 0).ok_or(WriteError::Source("System superblock"))?;
    if spec.name.len() >= APFS_VOLNAME_LEN || options.formatted_by.len() >= APFS_MODIFIED_NAMELEN {
        return Err(WriteError::Spec("volume name or formatted_by too long".into()));
    }
    let xid = input.ids.xid;
    let t = U64::new(options.time_ns);
    let mut sb = ApfsSuperblock::new_zeroed();
    sb.o = ObjPhys::new(input.ids.superblock_oid, xid, OBJ_VIRTUAL | TYPE_FS, 0);
    sb.magic = U32::new(APFS_MAGIC);
    sb.fs_index = U32::new(input.ids.fs_index);
    sb.features = U64::new(spec.features);
    sb.readonly_compatible_features = U64::new(spec.readonly_compatible_features);
    sb.incompatible_features = U64::new(APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | spec.incompatible_features);
    sb.unmount_time = t;
    sb.fs_alloc_count = U64::new(stats.metadata_blocks - 1 + stats.data_blocks);
    sb.meta_crypto = system_sb.meta_crypto;
    sb.root_tree_type = U32::new(OBJ_VIRTUAL | TYPE_BTREE);
    sb.extentref_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.snap_meta_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.omap_oid = U64::new(objects.omap);
    sb.root_tree_oid = U64::new(objects.root_tree_oid);
    sb.extentref_tree_oid = U64::new(objects.extent_ref);
    sb.snap_meta_tree_oid = U64::new(objects.snap_meta);
    sb.next_obj_id = U64::new(records.next_obj_id);
    sb.num_files = U64::new(records.files);
    sb.num_directories = U64::new(records.dirs);
    sb.num_symlinks = U64::new(records.symlinks);
    sb.total_blocks_alloced = U64::new(stats.data_blocks);
    sb.vol_uuid = spec.uuid;
    sb.last_mod_time = t;
    sb.fs_flags = U64::new(APFS_FS_UNENCRYPTED);
    let mut id = [0u8; APFS_MODIFIED_NAMELEN];
    id[..options.formatted_by.len()].copy_from_slice(options.formatted_by.as_bytes());
    sb.formatted_by = ModifiedBy {
        id,
        timestamp: t,
        last_xid: U64::new(xid),
    };
    sb.volname[..spec.name.len()].copy_from_slice(spec.name.as_bytes());
    sb.next_doc_id = U32::new(FIRST_DOC_ID.max(records.max_doc_id + 1));
    sb.role = U16::new(spec.role);
    sb.cloneinfo_id_epoch = U64::new(MIN_USER_INO_NUM);
    sb.cloneinfo_xid = U64::new(xid);
    sb.fext_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.reserved_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    block[APFS_SUPERBLOCK_END..].copy_from_slice(&input.system[APFS_SUPERBLOCK_END..BLOCK_SIZE]);
    put(&mut block[..], 0, &sb);
    put(&mut block[..], APFS_SUPERBLOCK_END, &U64::new(xid));
    seal(&mut block[..]);
    Ok(block)
}
