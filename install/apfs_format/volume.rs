// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::object::ObjPhys;

pub const APFS_MAGIC: u32 = u32::from_le_bytes(*b"APSB");
pub const APFS_MAX_HIST: usize = 8;
pub const APFS_VOLNAME_LEN: usize = 256;
pub const APFS_MODIFIED_NAMELEN: usize = 32;

pub const APFS_FS_UNENCRYPTED: u64 = 0x1;

pub const APFS_FEATURE_HARDLINK_MAP_RECORDS: u64 = 0x2;
pub const APFS_FEATURE_VOLGRP_SYSTEM_INO_SPACE: u64 = 0x10;

pub const APFS_INCOMPAT_CASE_INSENSITIVE: u64 = 0x1;
pub const APFS_INCOMPAT_NORMALIZATION_INSENSITIVE: u64 = 0x8;
pub const APFS_INCOMPAT_SEALED_VOLUME: u64 = 0x20;
/// Not in the 2020 Reference: iOS's formatter sets it on Data (tools/install/spec.py). A directory then keeps its
/// subdirectory count in `uncompressed_size`.
pub const APFS_INCOMPAT_EXPANDED_RECORDS: u64 = 0x200;

pub const APFS_VOL_ROLE_SYSTEM: u16 = 0x0001;
pub const APFS_VOL_ROLE_PREBOOT: u16 = 0x0010;
pub const APFS_VOL_ROLE_DATA: u16 = 0x0040;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug)]
#[repr(C)]
pub struct ModifiedBy {
    pub id: [u8; APFS_MODIFIED_NAMELEN],
    pub timestamp: U64,
    pub last_xid: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct WrappedMetaCryptoState {
    pub major_version: U16,
    pub minor_version: U16,
    pub cpflags: U32,
    pub persistent_class: U32,
    pub key_os_version: U32,
    pub key_revision: U16,
    pub unused: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug)]
#[repr(C)]
pub struct ApfsSuperblock {
    pub o: ObjPhys,
    pub magic: U32,
    pub fs_index: U32,
    pub features: U64,
    pub readonly_compatible_features: U64,
    pub incompatible_features: U64,
    pub unmount_time: U64,
    pub fs_reserve_block_count: U64,
    pub fs_quota_block_count: U64,
    pub fs_alloc_count: U64,
    pub meta_crypto: WrappedMetaCryptoState,
    pub root_tree_type: U32,
    pub extentref_tree_type: U32,
    pub snap_meta_tree_type: U32,
    pub omap_oid: U64,
    pub root_tree_oid: U64,
    pub extentref_tree_oid: U64,
    pub snap_meta_tree_oid: U64,
    pub revert_to_xid: U64,
    pub revert_to_sblock_oid: U64,
    pub next_obj_id: U64,
    pub num_files: U64,
    pub num_directories: U64,
    pub num_symlinks: U64,
    pub num_other_fsobjects: U64,
    pub num_snapshots: U64,
    pub total_blocks_alloced: U64,
    pub total_blocks_freed: U64,
    pub vol_uuid: [u8; 16],
    pub last_mod_time: U64,
    pub fs_flags: U64,
    pub formatted_by: ModifiedBy,
    pub modified_by: [ModifiedBy; APFS_MAX_HIST],
    pub volname: [u8; APFS_VOLNAME_LEN],
    pub next_doc_id: U32,
    pub role: U16,
    pub reserved: U16,
    pub root_to_xid: U64,
    pub er_state_oid: U64,
    pub cloneinfo_id_epoch: U64,
    pub cloneinfo_xid: U64,
    pub snap_meta_ext_oid: U64,
    pub volume_group_id: [u8; 16],
    pub integrity_meta_oid: U64,
    pub fext_tree_oid: U64,
    pub fext_tree_type: U32,
    pub reserved_type: U32,
    pub reserved_oid: U64,
}

/// Where the Reference's struct ends. Apple's kexts write more words after it; the first is the superblock's own
/// xid in every volume of the 24B119 and 21H461 OS images and of macOS 27.2.
pub const APFS_SUPERBLOCK_END: usize = size_of::<ApfsSuperblock>();
const _: () = assert!(APFS_SUPERBLOCK_END == 1056);

impl ApfsSuperblock {
    pub fn name(&self) -> String {
        let end = self.volname.iter().position(|&b| b == 0).unwrap_or(APFS_VOLNAME_LEN);
        String::from_utf8_lossy(&self.volname[..end]).into_owned()
    }
}
