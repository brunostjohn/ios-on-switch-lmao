// SPDX-License-Identifier: GPL-3.0-or-later
//! The fs-tree records and sealed-volume objects beyond `fs.rs`: xattrs, siblings, directory stats, file info, the
//! integrity object and the fext tree.

use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::fs::{Dstream, JKey};
use crate::object::ObjPhys;

pub const TYPE_INTEGRITY_META: u32 = 0x1E;
pub const TYPE_FEXT_TREE: u32 = 0x1F;

pub const APFS_TYPE_SNAP_METADATA: u8 = 1;
pub const APFS_TYPE_SIBLING_LINK: u8 = 5;
pub const APFS_TYPE_CRYPTO_STATE: u8 = 7;
pub const APFS_TYPE_DIR_STATS: u8 = 10;
pub const APFS_TYPE_SNAP_NAME: u8 = 11;
pub const APFS_TYPE_SIBLING_MAP: u8 = 12;
pub const APFS_TYPE_FILE_INFO: u8 = 13;

pub const S_IFMT: u16 = 0o170000;
pub const S_IFLNK: u16 = 0o120000;
pub const DREC_TYPE_MASK: u16 = 0x000F;

pub const XATTR_DATA_STREAM: u16 = 0x0001;
pub const XATTR_DATA_EMBEDDED: u16 = 0x0002;
pub const XATTR_FILE_SYSTEM_OWNED: u16 = 0x0004;
pub const XATTR_MAX_EMBEDDED_SIZE: usize = 3804;

pub const DREC_EXT_TYPE_SIBLING_ID: u8 = 1;
pub const INO_EXT_TYPE_SPARSE_BYTES: u8 = 13;
pub const INO_EXT_TYPE_RDEV: u8 = 14;

pub const UF_COMPRESSED: u32 = 0x0000_0020;

pub const J_FILE_INFO_LBA_MASK: u64 = 0x00FF_FFFF_FFFF_FFFF;
pub const J_FILE_INFO_TYPE_SHIFT: u32 = 56;
pub const APFS_FILE_INFO_DATA_HASH: u8 = 1;

pub const APFS_HASH_SHA256: u32 = 1;
pub const APFS_HASH_SHA256_SIZE: usize = 32;
/// An index entry of a hashed tree: the child's oid, relative to the root's, then the child node's hash.
pub const HASHED_INDEX_VALUE_SIZE: usize = 8 + APFS_HASH_SHA256_SIZE;

/// `com.apple.decmpfs`'s header, in front of any inline payload.
pub const DECMPFS_MAGIC: u32 = u32::from_le_bytes(*b"fpmc");
pub const DECMPFS_XATTR_NAME: &str = "com.apple.decmpfs";
pub const RESOURCE_FORK_XATTR_NAME: &str = "com.apple.ResourceFork";
pub const SYMLINK_XATTR_NAME: &str = "com.apple.fs.symlink";

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct XattrVal {
    pub flags: U16,
    pub xdata_len: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct XattrDstream {
    pub xattr_obj_id: U64,
    pub dstream: Dstream,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct SiblingKey {
    pub hdr: JKey,
    pub sibling_id: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct SiblingVal {
    pub parent_id: U64,
    pub name_len: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DirStatsVal {
    pub num_children: U64,
    pub total_size: U64,
    pub chained_key: U64,
    pub gen_count: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FileInfoKey {
    pub hdr: JKey,
    pub info_and_lba: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FileDataHashVal {
    pub hashed_len: U16,
    pub hash_size: u8,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct IntegrityMetaPhys {
    pub o: ObjPhys,
    pub version: U32,
    pub flags: U32,
    pub hash_type: U32,
    pub root_hash_offset: U32,
    pub broken_xid: U64,
    pub reserved: [U64; 9],
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FextKey {
    pub private_id: U64,
    pub logical_addr: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FextVal {
    pub len_and_flags: U64,
    pub phys_block_num: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DecmpfsHeader {
    pub magic: U32,
    pub compression_type: U32,
    pub uncompressed_size: U64,
}

pub const DECMPFS_HEADER_SIZE: usize = size_of::<DecmpfsHeader>();
const _: () = assert!(size_of::<IntegrityMetaPhys>() == 128);
const _: () = assert!(size_of::<XattrDstream>() == 48);
