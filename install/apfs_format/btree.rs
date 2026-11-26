// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

use crate::object::ObjPhys;

pub const BTNODE_ROOT: u16 = 0x0001;
pub const BTNODE_LEAF: u16 = 0x0002;
pub const BTNODE_FIXED_KV_SIZE: u16 = 0x0004;
pub const BTNODE_HASHED: u16 = 0x0008;
pub const BTNODE_NOHEADER: u16 = 0x0010;

pub const BTREE_UINT64_KEYS: u32 = 0x0001;
pub const BTREE_SEQUENTIAL_INSERT: u32 = 0x0002;
pub const BTREE_ALLOW_GHOSTS: u32 = 0x0004;
pub const BTREE_EPHEMERAL: u32 = 0x0008;
pub const BTREE_PHYSICAL: u32 = 0x0010;
pub const BTREE_NONPERSISTENT: u32 = 0x0020;
pub const BTREE_KV_NONALIGNED: u32 = 0x0040;
pub const BTREE_HASHED: u32 = 0x0080;
pub const BTREE_NOHEADER: u32 = 0x0100;

pub const BTOFF_INVALID: u16 = 0xFFFF;
pub const BTREE_TOC_ENTRY_INCREMENT: usize = 8;
pub const INDEX_VALUE_SIZE: usize = 8;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Nloc {
    pub off: U16,
    pub len: U16,
}

impl Nloc {
    pub const EMPTY_LIST: Nloc = Nloc {
        off: U16::new(BTOFF_INVALID),
        len: U16::ZERO,
    };

    pub fn new(off: u16, len: u16) -> Self {
        Self {
            off: U16::new(off),
            len: U16::new(len),
        }
    }
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct BtreeNodePhys {
    pub o: ObjPhys,
    pub flags: U16,
    pub level: U16,
    pub nkeys: U32,
    pub table_space: Nloc,
    pub free_space: Nloc,
    pub key_free_list: Nloc,
    pub val_free_list: Nloc,
}

pub const BTREE_NODE_DATA_AT: usize = size_of::<BtreeNodePhys>();
const _: () = assert!(BTREE_NODE_DATA_AT == 56);

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct BtreeInfo {
    pub flags: U32,
    pub node_size: U32,
    pub key_size: U32,
    pub val_size: U32,
    pub longest_key: U32,
    pub longest_val: U32,
    pub key_count: U64,
    pub node_count: U64,
}

pub const BTREE_INFO_SIZE: usize = size_of::<BtreeInfo>();

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Kvloc {
    pub k: Nloc,
    pub v: Nloc,
}

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Kvoff {
    pub k: U16,
    pub v: U16,
}
