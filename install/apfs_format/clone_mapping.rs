// SPDX-License-Identifier: GPL-3.0-or-later
//! The clone-mapping record, which the 2020 Reference doesn't describe. A volume with
//! `APFS_FEATURE_CLONE_MAPPING` (rocompat 0x4, which iOS sets on Data) keeps one for each inode of a whole-file clone:
//! inodes sharing one private id, which is a data stream's id, or a compressed file's whose clones have none. Layout
//! and values are as macOS 27.2's kext writes them (out/agent-scratch/creator/apfs-w4 probes); `fsck_apfs` names the
//! fields ("clone mapping (private_id, file_id, flags)", "physical size") and requires as many records per stream as
//! its `DSTREAM_ID` reference count.

use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::fs::JKey;

pub const APFS_ROCOMPAT_CLONE_MAPPING: u64 = 0x4;
pub const APFS_TYPE_CLONE_MAPPING: u8 = 14;
/// Every clone-mapping record sits under this object id.
pub const CLONE_MAPPING_OID: u64 = 9;
/// The only flags seen, on clones of plain and of compressed files alike.
pub const CLONE_MAPPING_FLAGS: u32 = 0x13;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct CloneMappingKey {
    pub hdr: JKey,
    pub flags: U32,
    pub private_id: U64,
    pub file_id: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct CloneMappingVal {
    /// Bytes of the shared stream's allocated (non-hole) extents; for compressed clones, the resource fork's, or 0
    /// when the decmpfs is in a stream of its own.
    pub physical_size: U64,
}

const _: () = assert!(size_of::<CloneMappingKey>() == 28);
