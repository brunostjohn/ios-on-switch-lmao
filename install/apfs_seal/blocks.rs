// SPDX-License-Identifier: GPL-3.0-or-later
use sha2::{Digest, Sha256};
use switch_apfs_alloc::Geometry;
use switch_apfs_btree::{Layout, Record, TreeShape, leaf_root};
use switch_apfs_format::container::{
    CHECKPOINT_MAP_LAST, CPM_ENTRIES_AT, CheckpointMapHeader, CheckpointMapping, NX_INCOMPAT_VERSION2, NX_MAGIC,
    NxSuperblock,
};
use switch_apfs_format::fs::MIN_USER_INO_NUM;
use switch_apfs_format::fs_records::{APFS_HASH_SHA256, APFS_HASH_SHA256_SIZE, IntegrityMetaPhys, TYPE_INTEGRITY_META};
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BTREE, TYPE_CHECKPOINT_MAP, TYPE_FS, TYPE_NX_REAPER,
    TYPE_NX_SUPERBLOCK, TYPE_SPACEMAN, TYPE_SPACEMAN_FREE_QUEUE,
};
use switch_apfs_format::omap::{OmapKey, OmapVal};
use switch_apfs_format::volume::{
    APFS_FS_UNENCRYPTED, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_INCOMPAT_SEALED_VOLUME, APFS_MAGIC,
    APFS_MODIFIED_NAMELEN, APFS_SUPERBLOCK_END, ApfsSuperblock, ModifiedBy,
};
use switch_apfs_format::{BLOCK_SIZE, Block, get, put, seal};
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromZeros, IntoBytes};

use crate::source::SourceCryptex;
use crate::{SealError, SealOptions};

pub const SEAL_XID: u64 = 1;
pub const SPACEMAN_OID: u64 = 0x400;
pub const REAPER_OID: u64 = 0x401;
pub const FQ_IP_OID: u64 = 0x402;
pub const FQ_MAIN_OID: u64 = 0x403;
pub const VOL_OID: u64 = 0x404;
pub const INTEGRITY_OID: u64 = 0x405;
pub const FS_ROOT_OID: u64 = 0x406;

const INTEGRITY_META_VERSION_SEALED: u32 = 2;
const NR_BHM_FLAG: u32 = 0x1;
const NR_STATE_BUFFER_SIZE: u32 = (BLOCK_SIZE - 112) as u32;
const VSB_MODIFIED_XID_AT: usize = 1120;

pub struct VolumeCounts {
    pub next_obj_id: u64,
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub other: u64,
    pub data_blocks: u64,
}

pub struct VolumeRefs {
    pub omap: u64,
    pub root_tree_oid: u64,
    pub extentref: u64,
    pub snap_meta: u64,
    pub integrity_oid: u64,
    pub fext: u64,
    pub fs_alloc_count: u64,
}

pub fn omap_entry(oid: u64, xid: u64, flags: u32, paddr: u64) -> Record {
    let key = OmapKey {
        oid: U64::new(oid),
        xid: U64::new(xid),
    };
    let val = OmapVal {
        flags: U32::new(flags),
        size: U32::new(BLOCK_SIZE as u32),
        paddr: U64::new(paddr),
    };
    (key.as_bytes().to_vec(), Some(val.as_bytes().to_vec()))
}

pub fn emit_physical(layout: &Layout, xid: u64, addrs: &[u64]) -> Vec<(u64, Box<Block>)> {
    let mut next = addrs.iter().copied();
    let built = layout.emit(xid, &mut || next.next().unwrap_or(0));
    built.nodes.into_iter().map(|n| (n.id, n.block)).collect()
}

pub fn integrity_block(oid: u64, xid: u64, root_hash: &[u8; APFS_HASH_SHA256_SIZE]) -> Box<Block> {
    let meta = IntegrityMetaPhys {
        o: ObjPhys::new(oid, xid, OBJ_VIRTUAL | TYPE_INTEGRITY_META, 0),
        version: U32::new(INTEGRITY_META_VERSION_SEALED),
        flags: U32::ZERO,
        hash_type: U32::new(APFS_HASH_SHA256),
        root_hash_offset: U32::new(size_of::<IntegrityMetaPhys>() as u32),
        broken_xid: U64::ZERO,
        reserved: [U64::ZERO; 9],
    };
    let mut block = Box::new([0u8; BLOCK_SIZE]);
    put(&mut block[..], 0, &meta);
    let at = size_of::<IntegrityMetaPhys>();
    block[at..at + APFS_HASH_SHA256_SIZE].copy_from_slice(root_hash);
    seal(&mut block[..]);
    block
}

pub fn reaper_block(oid: u64, xid: u64) -> Box<Block> {
    let mut block = Box::new([0u8; BLOCK_SIZE]);
    put(
        &mut block[..],
        0,
        &ObjPhys::new(oid, xid, OBJ_EPHEMERAL | TYPE_NX_REAPER, 0),
    );
    put(&mut block[..], 32, &U64::new(1));
    put(&mut block[..], 64, &U32::new(NR_BHM_FLAG));
    put(&mut block[..], 108, &U32::new(NR_STATE_BUFFER_SIZE));
    seal(&mut block[..]);
    block
}

pub fn checkpoint_blocks(
    desc_base: u64,
    data_base: u64,
    xid: u64,
    spaceman: Box<Block>,
) -> Result<Vec<(u64, Box<Block>)>, SealError> {
    let ephemeral = [
        (
            data_base,
            OBJ_EPHEMERAL | TYPE_NX_REAPER,
            0u32,
            REAPER_OID,
            reaper_block(REAPER_OID, xid),
        ),
        (data_base + 1, OBJ_EPHEMERAL | TYPE_SPACEMAN, 0, SPACEMAN_OID, spaceman),
        (
            data_base + 2,
            OBJ_EPHEMERAL | TYPE_BTREE,
            TYPE_SPACEMAN_FREE_QUEUE,
            FQ_IP_OID,
            leaf_root(&TreeShape::FREE_QUEUE, FQ_IP_OID, xid, &[])?,
        ),
        (
            data_base + 3,
            OBJ_EPHEMERAL | TYPE_BTREE,
            TYPE_SPACEMAN_FREE_QUEUE,
            FQ_MAIN_OID,
            leaf_root(&TreeShape::FREE_QUEUE, FQ_MAIN_OID, xid, &[])?,
        ),
    ];
    let mut cpm = Box::new([0u8; BLOCK_SIZE]);
    put(
        &mut cpm[..],
        0,
        &CheckpointMapHeader {
            o: ObjPhys::new(desc_base, xid, OBJ_PHYSICAL | TYPE_CHECKPOINT_MAP, 0),
            flags: U32::new(CHECKPOINT_MAP_LAST),
            count: U32::new(ephemeral.len() as u32),
        },
    );
    let mut out = Vec::with_capacity(ephemeral.len() + 1);
    for (k, (paddr, type_and_flags, subtype, oid, block)) in ephemeral.into_iter().enumerate() {
        let m = CheckpointMapping {
            type_and_flags: U32::new(type_and_flags),
            subtype: U32::new(subtype),
            size: U32::new(BLOCK_SIZE as u32),
            pad: U32::ZERO,
            fs_oid: U64::ZERO,
            oid: U64::new(oid),
            paddr: U64::new(paddr),
        };
        put(&mut cpm[..], CPM_ENTRIES_AT + k * size_of::<CheckpointMapping>(), &m);
        out.push((paddr, block));
    }
    seal(&mut cpm[..]);
    out.push((desc_base, cpm));
    Ok(out)
}

pub fn volume_superblock(
    cryptex: &SourceCryptex,
    options: &SealOptions,
    name: &str,
    counts: &VolumeCounts,
    refs: VolumeRefs,
) -> Box<Block> {
    let src_sb = &cryptex.sb;
    let t = U64::new(options.time_ns);
    let mut sb = ApfsSuperblock::new_zeroed();
    sb.o = ObjPhys::new(VOL_OID, SEAL_XID, OBJ_VIRTUAL | TYPE_FS, 0);
    sb.magic = U32::new(APFS_MAGIC);
    sb.fs_index = U32::ZERO;
    sb.features = src_sb.features;
    sb.readonly_compatible_features = src_sb.readonly_compatible_features;
    sb.incompatible_features = U64::new(
        src_sb.incompatible_features.get() | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | APFS_INCOMPAT_SEALED_VOLUME,
    );
    sb.unmount_time = t;
    sb.fs_alloc_count = U64::new(refs.fs_alloc_count);
    sb.meta_crypto = src_sb.meta_crypto;
    sb.root_tree_type = U32::new(OBJ_VIRTUAL | TYPE_BTREE);
    sb.extentref_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.snap_meta_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.omap_oid = U64::new(refs.omap);
    sb.root_tree_oid = U64::new(refs.root_tree_oid);
    sb.extentref_tree_oid = U64::new(refs.extentref);
    sb.snap_meta_tree_oid = U64::new(refs.snap_meta);
    sb.next_obj_id = U64::new(counts.next_obj_id);
    sb.num_files = U64::new(counts.files);
    sb.num_directories = U64::new(counts.dirs);
    sb.num_symlinks = U64::new(counts.symlinks);
    sb.num_other_fsobjects = U64::new(counts.other);
    sb.total_blocks_alloced = U64::new(counts.data_blocks);
    sb.vol_uuid = options.volume_uuid;
    sb.last_mod_time = t;
    sb.fs_flags = U64::new(APFS_FS_UNENCRYPTED);
    let mut id = [0u8; APFS_MODIFIED_NAMELEN];
    id[..options.formatted_by.len()].copy_from_slice(options.formatted_by.as_bytes());
    let entry = ModifiedBy {
        id,
        timestamp: t,
        last_xid: U64::new(SEAL_XID),
    };
    sb.formatted_by = entry;
    sb.modified_by[0] = entry;
    sb.volname[..name.len()].copy_from_slice(name.as_bytes());
    sb.next_doc_id = U32::new(src_sb.next_doc_id.get().max(3));
    sb.role = U16::new(src_sb.role.get());
    sb.cloneinfo_id_epoch = U64::new(src_sb.cloneinfo_id_epoch.get().max(MIN_USER_INO_NUM));
    sb.cloneinfo_xid = U64::new(SEAL_XID);
    sb.volume_group_id = src_sb.volume_group_id;
    sb.integrity_meta_oid = U64::new(refs.integrity_oid);
    sb.fext_tree_oid = U64::new(refs.fext);
    sb.fext_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.reserved_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    block[APFS_SUPERBLOCK_END..].copy_from_slice(&cryptex.sb_block[APFS_SUPERBLOCK_END..BLOCK_SIZE]);
    put(&mut block[..], 0, &sb);
    put(&mut block[..], APFS_SUPERBLOCK_END, &U64::new(SEAL_XID));
    if get::<U64>(&cryptex.sb_block[..], VSB_MODIFIED_XID_AT).is_some_and(|w| w.get() != 0) {
        put(&mut block[..], VSB_MODIFIED_XID_AT, &U64::new(SEAL_XID));
    }
    seal(&mut block[..]);
    block
}

pub fn nx_superblock(
    cryptex: &SourceCryptex,
    g: &Geometry,
    vol_uuid: [u8; 16],
    next_oid: u64,
    desc_base: u64,
    data_base: u64,
    omap_paddr: u64,
) -> Box<Block> {
    let mut nx = NxSuperblock::new_zeroed();
    nx.o = ObjPhys::new(1, SEAL_XID, OBJ_EPHEMERAL | TYPE_NX_SUPERBLOCK, 0);
    nx.magic = U32::new(NX_MAGIC);
    nx.block_size = U32::new(BLOCK_SIZE as u32);
    nx.block_count = U64::new(g.blocks);
    nx.incompatible_features = U64::new(NX_INCOMPAT_VERSION2);
    nx.uuid = deterministic_nx_uuid(&vol_uuid, &cryptex.nx.uuid);
    nx.next_oid = U64::new(next_oid);
    nx.next_xid = U64::new(SEAL_XID + 1);
    nx.xp_desc_blocks = U32::new(g.checkpoint.desc_blocks);
    nx.xp_data_blocks = U32::new(g.checkpoint.data_blocks);
    nx.xp_desc_base = U64::new(desc_base);
    nx.xp_data_base = U64::new(data_base);
    nx.xp_desc_next = U32::new(2);
    nx.xp_data_next = U32::new(4);
    nx.xp_desc_index = U32::ZERO;
    nx.xp_desc_len = U32::new(2);
    nx.xp_data_index = U32::ZERO;
    nx.xp_data_len = U32::new(4);
    nx.spaceman_oid = U64::new(SPACEMAN_OID);
    nx.omap_oid = U64::new(omap_paddr);
    nx.reaper_oid = U64::new(REAPER_OID);
    nx.max_file_systems = U32::new(g.max_file_systems);
    nx.fs_oid[0] = U64::new(VOL_OID);
    nx.ephemeral_info = cryptex.nx.ephemeral_info;
    nx.newest_mounted_version = cryptex.nx.newest_mounted_version;

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    put(&mut block[..], 0, &nx);
    seal(&mut block[..]);
    block
}

fn deterministic_nx_uuid(vol_uuid: &[u8; 16], src_nx_uuid: &[u8; 16]) -> [u8; 16] {
    let mut h = Sha256::new();
    h.update(b"switch-apfs-seal-nx-uuid");
    h.update(vol_uuid);
    h.update(src_nx_uuid);
    let digest = h.finalize();
    let mut out: [u8; 16] = digest[..16].try_into().expect("16 bytes");
    out[6] = (out[6] & 0x0F) | 0x40;
    out[8] = (out[8] & 0x3F) | 0x80;
    out
}
