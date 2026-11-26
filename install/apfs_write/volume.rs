// SPDX-License-Identifier: GPL-3.0-or-later
//! A new volume's own objects: superblock, object map, fs tree, extent-reference tree and snapshot-metadata tree.

use switch_apfs_btree::{TreeShape, leaf_root};
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
use crate::records::{self, Numbered, RecordInput};
use crate::spec::{Tree, VolumeSpec, WriteOptions};

/// Blocks a volume's own objects take besides file data: superblock, object map and its tree, fs-tree root,
/// extent-reference root, snapshot-metadata root.
pub const METADATA_BLOCKS: u64 = 6;

pub struct VolumeAddrs {
    pub superblock: u64,
    pub omap: u64,
    pub omap_tree: u64,
    pub fs_root: u64,
    pub extent_ref: u64,
    pub snap_meta: u64,
    pub extents: Vec<u64>,
}

impl VolumeAddrs {
    pub fn from_metadata_run(first: u64, extents: Vec<u64>) -> Self {
        Self {
            superblock: first,
            omap: first + 1,
            omap_tree: first + 2,
            fs_root: first + 3,
            extent_ref: first + 4,
            snap_meta: first + 5,
            extents,
        }
    }
}

pub struct VolumeIds {
    pub fs_index: u32,
    pub superblock_oid: u64,
    pub root_tree_oid: u64,
    pub xid: u64,
}

pub struct BuiltVolume {
    pub blocks: Vec<(u64, Box<Block>)>,
    /// File data: each extent's first block and bytes (zero-padded to whole blocks).
    pub data: Vec<(u64, Vec<u8>)>,
}

#[allow(clippy::too_many_arguments)]
pub fn build(
    spec: &VolumeSpec,
    tree: &Tree,
    numbered: &Numbered,
    ids: &VolumeIds,
    addrs: &VolumeAddrs,
    options: &WriteOptions,
    system: &[u8],
) -> Result<BuiltVolume, WriteError> {
    let xid = ids.xid;
    let incompat = APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | spec.incompatible_features;
    let (fs_records, extent_records) = records::build(&RecordInput {
        tree,
        numbered,
        extent_addrs: &addrs.extents,
        incompatible_features: incompat,
        unicode: spec.unicode,
        time_ns: options.time_ns,
    })?;
    let fs_root = leaf_root(&TreeShape::FS_TREE, ids.root_tree_oid, xid, &fs_records)?;
    let extent_ref = leaf_root(&TreeShape::EXTENT_REF, addrs.extent_ref, xid, &extent_records)?;
    let snap_meta = leaf_root(&TreeShape::SNAP_META, addrs.snap_meta, xid, &[])?;
    let omap_entry = (
        OmapKey {
            oid: U64::new(ids.root_tree_oid),
            xid: U64::new(xid),
        }
        .as_bytes()
        .to_vec(),
        Some(
            OmapVal {
                flags: U32::ZERO,
                size: U32::new(BLOCK_SIZE as u32),
                paddr: U64::new(addrs.fs_root),
            }
            .as_bytes()
            .to_vec(),
        ),
    );
    let omap_tree = leaf_root(&TreeShape::OMAP, addrs.omap_tree, xid, &[omap_entry])?;
    let omap = omap_block(addrs.omap, addrs.omap_tree, xid, 0);

    let extents = numbered.extents();
    let data_blocks: u64 = extents.iter().map(|e| e.blocks).sum();
    let superblock = superblock(
        spec,
        numbered,
        ids,
        addrs,
        options,
        system,
        METADATA_BLOCKS - 1 + data_blocks,
    )?;

    let mut data = Vec::new();
    for (extent, &paddr) in extents.iter().zip(&addrs.extents) {
        let bytes = numbered
            .nodes
            .iter()
            .find(|n| n.inode == extent.inode)
            .and_then(|n| match &n.entry.content {
                crate::spec::Content::File(bytes) => Some(bytes),
                crate::spec::Content::Dir(_) => None,
            });
        let mut padded = bytes.cloned().unwrap_or_default();
        padded.resize((extent.blocks as usize) * BLOCK_SIZE, 0);
        data.push((paddr, padded));
    }
    Ok(BuiltVolume {
        blocks: vec![
            (addrs.superblock, superblock),
            (addrs.omap, omap),
            (addrs.omap_tree, omap_tree),
            (addrs.fs_root, fs_root),
            (addrs.extent_ref, extent_ref),
            (addrs.snap_meta, snap_meta),
        ],
        data,
    })
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

/// The words Apple's kext keeps past the Reference's struct are taken from the System volume of the same IPSW, as
/// if that kext had formatted this volume too; the first is the superblock's own xid.
fn superblock(
    spec: &VolumeSpec,
    numbered: &Numbered,
    ids: &VolumeIds,
    addrs: &VolumeAddrs,
    options: &WriteOptions,
    system: &[u8],
    alloc_count: u64,
) -> Result<Box<Block>, WriteError> {
    let system_sb: ApfsSuperblock =
        switch_apfs_format::get(system, 0).ok_or(WriteError::Source("System superblock"))?;
    if spec.name.len() >= APFS_VOLNAME_LEN || options.formatted_by.len() >= APFS_MODIFIED_NAMELEN {
        return Err(WriteError::Spec("volume name or formatted_by too long".into()));
    }
    let xid = ids.xid;
    let t = U64::new(options.time_ns);
    let mut sb = ApfsSuperblock::new_zeroed();
    sb.o = ObjPhys::new(ids.superblock_oid, xid, OBJ_VIRTUAL | TYPE_FS, 0);
    sb.magic = U32::new(APFS_MAGIC);
    sb.fs_index = U32::new(ids.fs_index);
    sb.features = U64::new(spec.features);
    sb.readonly_compatible_features = U64::new(spec.readonly_compatible_features);
    sb.incompatible_features = U64::new(APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | spec.incompatible_features);
    sb.unmount_time = t;
    sb.fs_alloc_count = U64::new(alloc_count);
    sb.meta_crypto = system_sb.meta_crypto;
    sb.root_tree_type = U32::new(OBJ_VIRTUAL | TYPE_BTREE);
    sb.extentref_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.snap_meta_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.omap_oid = U64::new(addrs.omap);
    sb.root_tree_oid = U64::new(ids.root_tree_oid);
    sb.extentref_tree_oid = U64::new(addrs.extent_ref);
    sb.snap_meta_tree_oid = U64::new(addrs.snap_meta);
    sb.next_obj_id = U64::new(numbered.next_inode);
    sb.num_files = U64::new(numbered.files);
    sb.num_directories = U64::new(numbered.dirs);
    sb.total_blocks_alloced = U64::new(numbered.extents().iter().map(|e| e.blocks).sum());
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
    sb.next_doc_id = U32::new(3);
    sb.role = U16::new(spec.role);
    sb.cloneinfo_id_epoch = U64::new(MIN_USER_INO_NUM);
    sb.cloneinfo_xid = U64::new(xid);
    sb.fext_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.reserved_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    block[APFS_SUPERBLOCK_END..].copy_from_slice(&system[APFS_SUPERBLOCK_END..BLOCK_SIZE]);
    put(&mut block[..], 0, &sb);
    put(&mut block[..], APFS_SUPERBLOCK_END, &U64::new(xid));
    seal(&mut block[..]);
    Ok(block)
}
