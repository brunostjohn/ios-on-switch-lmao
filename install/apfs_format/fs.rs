// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{I32, U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

pub const OBJ_ID_MASK: u64 = 0x0FFF_FFFF_FFFF_FFFF;
pub const OBJ_TYPE_SHIFT: u32 = 60;

pub const APFS_TYPE_EXTENT: u8 = 2;
pub const APFS_TYPE_INODE: u8 = 3;
pub const APFS_TYPE_XATTR: u8 = 4;
pub const APFS_TYPE_DSTREAM_ID: u8 = 6;
pub const APFS_TYPE_FILE_EXTENT: u8 = 8;
pub const APFS_TYPE_DIR_REC: u8 = 9;

pub const ROOT_DIR_PARENT: u64 = 1;
pub const ROOT_DIR_INO_NUM: u64 = 2;
pub const PRIV_DIR_INO_NUM: u64 = 3;
pub const MIN_USER_INO_NUM: u64 = 16;

pub const ROOT_DIR_NAME: &str = "root";
pub const PRIV_DIR_NAME: &str = "private-dir";

pub const INODE_NO_RSRC_FORK: u64 = 0x8000;

pub const DT_DIR: u16 = 4;
pub const DT_REG: u16 = 8;
pub const DT_LNK: u16 = 10;

pub const S_IFDIR: u16 = 0o040000;
pub const S_IFREG: u16 = 0o100000;

pub const J_DREC_LEN_MASK: u32 = 0x0000_03FF;
pub const J_DREC_HASH_SHIFT: u32 = 10;

pub const INO_EXT_TYPE_NAME: u8 = 4;
pub const INO_EXT_TYPE_DSTREAM: u8 = 8;
pub const XF_DO_NOT_COPY: u8 = 0x02;
pub const XF_SYSTEM_FIELD: u8 = 0x20;

pub const APFS_KIND_NEW: u64 = 1;
pub const PEXT_LEN_MASK: u64 = 0x0FFF_FFFF_FFFF_FFFF;
pub const PEXT_KIND_SHIFT: u32 = 60;
pub const J_FILE_EXTENT_LEN_MASK: u64 = 0x00FF_FFFF_FFFF_FFFF;

pub fn obj_id_and_type(oid: u64, kind: u8) -> u64 {
    (oid & OBJ_ID_MASK) | (u64::from(kind) << OBJ_TYPE_SHIFT)
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct JKey {
    pub obj_id_and_type: U64,
}

impl JKey {
    pub fn new(oid: u64, kind: u8) -> Self {
        Self {
            obj_id_and_type: U64::new(obj_id_and_type(oid, kind)),
        }
    }

    pub fn oid(&self) -> u64 {
        self.obj_id_and_type.get() & OBJ_ID_MASK
    }

    pub fn kind(&self) -> u8 {
        (self.obj_id_and_type.get() >> OBJ_TYPE_SHIFT) as u8
    }
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct InodeVal {
    pub parent_id: U64,
    pub private_id: U64,
    pub create_time: U64,
    pub mod_time: U64,
    pub change_time: U64,
    pub access_time: U64,
    pub internal_flags: U64,
    pub nchildren_or_nlink: I32,
    pub default_protection_class: U32,
    pub write_generation_counter: U32,
    pub bsd_flags: U32,
    pub owner: U32,
    pub group: U32,
    pub mode: U16,
    pub pad1: U16,
    /// On a volume with `APFS_INCOMPAT_EXPANDED_RECORDS`, a directory keeps its subdirectory count here (seen on
    /// macOS 27.2); files keep the Reference's uncompressed size.
    pub uncompressed_size: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DrecVal {
    pub file_id: U64,
    pub date_added: U64,
    pub flags: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct XfBlob {
    pub num_exts: U16,
    pub used_data: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct XField {
    pub kind: u8,
    pub flags: u8,
    pub size: U16,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Dstream {
    pub size: U64,
    pub alloced_size: U64,
    pub default_crypto_id: U64,
    pub total_bytes_written: U64,
    pub total_bytes_read: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct DstreamIdVal {
    pub refcnt: U32,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FileExtentKey {
    pub hdr: JKey,
    pub logical_addr: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FileExtentVal {
    pub len_and_flags: U64,
    pub phys_block_num: U64,
    pub crypto_id: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct PhysExtVal {
    pub len_and_kind: U64,
    pub owning_obj_id: U64,
    pub refcnt: I32,
}
