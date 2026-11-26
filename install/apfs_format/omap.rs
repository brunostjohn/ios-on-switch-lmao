// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::object::ObjPhys;

pub const OMAP_MANUALLY_MANAGED: u32 = 0x1;
pub const OMAP_VAL_DELETED: u32 = 0x1;
pub const OMAP_VAL_NOHEADER: u32 = 0x8;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct OmapPhys {
    pub o: ObjPhys,
    pub flags: U32,
    pub snap_count: U32,
    pub tree_type: U32,
    pub snapshot_tree_type: U32,
    pub tree_oid: U64,
    pub snapshot_tree_oid: U64,
    pub most_recent_snap: U64,
    pub pending_revert_min: U64,
    pub pending_revert_max: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct OmapKey {
    pub oid: U64,
    pub xid: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct OmapVal {
    pub flags: U32,
    pub size: U32,
    pub paddr: U64,
}

pub const OMAP_KEY_SIZE: usize = size_of::<OmapKey>();
pub const OMAP_VAL_SIZE: usize = size_of::<OmapVal>();
