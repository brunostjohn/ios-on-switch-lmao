// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::object::ObjPhys;

pub const NX_MAGIC: u32 = u32::from_le_bytes(*b"NXSB");
pub const NX_MAX_FILE_SYSTEMS: usize = 100;
pub const NX_NUM_COUNTERS: usize = 32;
pub const NX_EPH_INFO_COUNT: usize = 4;
pub const NX_INCOMPAT_VERSION2: u64 = 0x2;
pub const CHECKPOINT_MAP_LAST: u32 = 0x1;
pub const CPM_PER_BLOCK: usize = (crate::BLOCK_SIZE - 40) / 40;
/// The Reference describes this bit of `nx_xp_desc_blocks` and `nx_xp_data_blocks` without naming it.
pub const XP_NONCONTIGUOUS: u32 = 0x8000_0000;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Prange {
    pub start: U64,
    pub count: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug)]
#[repr(C)]
pub struct NxSuperblock {
    pub o: ObjPhys,
    pub magic: U32,
    pub block_size: U32,
    pub block_count: U64,
    pub features: U64,
    pub readonly_compatible_features: U64,
    pub incompatible_features: U64,
    pub uuid: [u8; 16],
    pub next_oid: U64,
    pub next_xid: U64,
    pub xp_desc_blocks: U32,
    pub xp_data_blocks: U32,
    pub xp_desc_base: U64,
    pub xp_data_base: U64,
    pub xp_desc_next: U32,
    pub xp_data_next: U32,
    pub xp_desc_index: U32,
    pub xp_desc_len: U32,
    pub xp_data_index: U32,
    pub xp_data_len: U32,
    pub spaceman_oid: U64,
    pub omap_oid: U64,
    pub reaper_oid: U64,
    pub test_type: U32,
    pub max_file_systems: U32,
    pub fs_oid: [U64; NX_MAX_FILE_SYSTEMS],
    pub counters: [U64; NX_NUM_COUNTERS],
    pub blocked_out_prange: Prange,
    pub evict_mapping_tree_oid: U64,
    pub flags: U64,
    pub efi_jumpstart: U64,
    pub fusion_uuid: [u8; 16],
    pub keylocker: Prange,
    pub ephemeral_info: [U64; NX_EPH_INFO_COUNT],
    pub test_oid: U64,
    pub fusion_mt_oid: U64,
    pub fusion_wbc_oid: U64,
    pub fusion_wbc: Prange,
    pub newest_mounted_version: U64,
    pub mkb_locker: Prange,
}

const _: () = assert!(size_of::<NxSuperblock>() == 1408);

impl NxSuperblock {
    pub fn volumes(&self) -> impl Iterator<Item = (usize, u64)> + '_ {
        self.fs_oid
            .iter()
            .enumerate()
            .map(|(i, o)| (i, o.get()))
            .filter(|(_, o)| *o != 0)
    }
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct CheckpointMapping {
    pub type_and_flags: U32,
    pub subtype: U32,
    pub size: U32,
    pub pad: U32,
    pub fs_oid: U64,
    pub oid: U64,
    pub paddr: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct CheckpointMapHeader {
    pub o: ObjPhys,
    pub flags: U32,
    pub count: U32,
}

pub const CPM_ENTRIES_AT: usize = size_of::<CheckpointMapHeader>();
