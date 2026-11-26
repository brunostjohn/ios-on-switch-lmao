// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::object::ObjPhys;

pub const SD_MAIN: usize = 0;
pub const SFQ_IP: usize = 0;
pub const SFQ_MAIN: usize = 1;
pub const SFQ_COUNT: usize = 3;
pub const SM_FLAG_VERSIONED: u32 = 0x1;
pub const SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES: usize = 7;
pub const SM_DATAZONE_ALLOCZONE_COUNT: usize = 8;
pub const SD_COUNT: usize = 2;
pub const CI_COUNT_MASK: u32 = 0x000F_FFFF;
pub const CHUNK_INFO_AT: usize = size_of::<CibHeader>();
/// Not a Reference name: the end of the pool-bitmap ring's free list in `sm_ip_bm_free_next`, as Apple writes it.
pub const SM_IP_BITMAP_RING_FREE_END: u16 = 0xFFFF;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct SpacemanDevice {
    pub block_count: U64,
    pub chunk_count: U64,
    pub cib_count: U32,
    pub cab_count: U32,
    pub free_count: U64,
    pub addr_offset: U32,
    pub reserved: U32,
    pub reserved2: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct SpacemanFreeQueue {
    pub count: U64,
    pub tree_oid: U64,
    pub oldest_xid: U64,
    pub tree_node_limit: U16,
    pub pad16: U16,
    pub pad32: U32,
    pub reserved: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct AllocationZoneBoundaries {
    pub start: U64,
    pub end: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct AllocationZoneInfo {
    pub current: AllocationZoneBoundaries,
    pub previous: [AllocationZoneBoundaries; SM_ALLOCZONE_NUM_PREVIOUS_BOUNDARIES],
    pub zone_id: U16,
    pub previous_boundary_index: U16,
    pub reserved: U32,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug)]
#[repr(C)]
pub struct SpacemanPhys {
    pub o: ObjPhys,
    pub block_size: U32,
    pub blocks_per_chunk: U32,
    pub chunks_per_cib: U32,
    pub cibs_per_cab: U32,
    pub dev: [SpacemanDevice; SD_COUNT],
    pub flags: U32,
    pub ip_bm_tx_multiplier: U32,
    pub ip_block_count: U64,
    pub ip_bm_size_in_blocks: U32,
    pub ip_bm_block_count: U32,
    pub ip_bm_base: U64,
    pub ip_base: U64,
    pub fs_reserve_block_count: U64,
    pub fs_reserve_alloc_count: U64,
    pub fq: [SpacemanFreeQueue; SFQ_COUNT],
    pub ip_bm_free_head: U16,
    pub ip_bm_free_tail: U16,
    pub ip_bm_xid_offset: U32,
    pub ip_bitmap_offset: U32,
    pub ip_bm_free_next_offset: U32,
    pub version: U32,
    pub struct_size: U32,
    pub datazone: [[AllocationZoneInfo; SM_DATAZONE_ALLOCZONE_COUNT]; SD_COUNT],
}

pub const SPACEMAN_STRUCT_SIZE: usize = size_of::<SpacemanPhys>();
const _: () = assert!(SPACEMAN_STRUCT_SIZE == 0x9D8);

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct ChunkInfo {
    pub xid: U64,
    pub addr: U64,
    pub block_count: U32,
    pub free_count: U32,
    pub bitmap_addr: U64,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct CibHeader {
    pub o: ObjPhys,
    pub index: U32,
    pub chunk_info_count: U32,
}

pub const CHUNK_INFOS_PER_CIB: usize = (crate::BLOCK_SIZE - CHUNK_INFO_AT) / size_of::<ChunkInfo>();

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FreeQueueKey {
    pub xid: U64,
    pub paddr: U64,
}
