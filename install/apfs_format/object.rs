// SPDX-License-Identifier: GPL-3.0-or-later
use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

pub const BLOCK_SIZE: usize = 4096;
pub type Block = [u8; BLOCK_SIZE];

pub const OBJ_VIRTUAL: u32 = 0x0000_0000;
pub const OBJ_EPHEMERAL: u32 = 0x8000_0000;
pub const OBJ_PHYSICAL: u32 = 0x4000_0000;
pub const OBJ_STORAGETYPE_MASK: u32 = 0xC000_0000;
pub const OBJECT_TYPE_MASK: u32 = 0x0000_FFFF;

pub const TYPE_NX_SUPERBLOCK: u32 = 0x01;
pub const TYPE_BTREE: u32 = 0x02;
pub const TYPE_BTREE_NODE: u32 = 0x03;
pub const TYPE_SPACEMAN: u32 = 0x05;
pub const TYPE_SPACEMAN_CAB: u32 = 0x06;
pub const TYPE_SPACEMAN_CIB: u32 = 0x07;
pub const TYPE_SPACEMAN_FREE_QUEUE: u32 = 0x09;
pub const TYPE_OMAP: u32 = 0x0B;
pub const TYPE_CHECKPOINT_MAP: u32 = 0x0C;
pub const TYPE_FS: u32 = 0x0D;
pub const TYPE_FSTREE: u32 = 0x0E;
pub const TYPE_BLOCKREFTREE: u32 = 0x0F;
pub const TYPE_SNAPMETATREE: u32 = 0x10;
pub const TYPE_NX_REAPER: u32 = 0x11;
pub const TYPE_NX_REAP_LIST: u32 = 0x12;

#[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned, Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct ObjPhys {
    pub cksum: U64,
    pub oid: U64,
    pub xid: U64,
    pub type_and_flags: U32,
    pub subtype: U32,
}

impl ObjPhys {
    pub fn new(oid: u64, xid: u64, type_and_flags: u32, subtype: u32) -> Self {
        Self {
            cksum: U64::ZERO,
            oid: U64::new(oid),
            xid: U64::new(xid),
            type_and_flags: U32::new(type_and_flags),
            subtype: U32::new(subtype),
        }
    }

    pub fn object_type(&self) -> u32 {
        self.type_and_flags.get() & OBJECT_TYPE_MASK
    }

    pub fn storage(&self) -> u32 {
        self.type_and_flags.get() & OBJ_STORAGETYPE_MASK
    }
}

pub fn header(block: &[u8]) -> ObjPhys {
    ObjPhys::read_from_prefix(block).map(|(h, _)| h).unwrap_or_default()
}

pub fn fletcher64(block: &[u8]) -> u64 {
    const MOD: u64 = 0xFFFF_FFFF;
    let (words, _) = block[8..].as_chunks::<4>();
    // Reducing once per 1024 words is the same sum modulo 2^32 - 1, and keeps both sums far below 2^64.
    let mut lo: u64 = 0;
    let mut hi: u64 = 0;
    for chunk in words.chunks(1024) {
        for word in chunk {
            lo += u64::from(u32::from_le_bytes(*word));
            hi += lo;
        }
        lo %= MOD;
        hi %= MOD;
    }
    lo %= MOD;
    hi %= MOD;
    let c1 = MOD - (lo + hi) % MOD;
    let c2 = MOD - (lo + c1) % MOD;
    (c2 << 32) | c1
}

pub fn checksum_ok(block: &[u8]) -> bool {
    block.len() >= 8 && u64::from_le_bytes(block[..8].try_into().unwrap_or_default()) == fletcher64(block)
}

pub fn seal(block: &mut [u8]) {
    let sum = fletcher64(block);
    block[..8].copy_from_slice(&sum.to_le_bytes());
}

pub fn put<T: IntoBytes + Immutable>(block: &mut [u8], at: usize, value: &T) {
    let bytes = value.as_bytes();
    block[at..at + bytes.len()].copy_from_slice(bytes);
}

pub fn get<T: FromBytes>(block: &[u8], at: usize) -> Option<T> {
    T::read_from_prefix(block.get(at..)?).ok().map(|(v, _)| v)
}
