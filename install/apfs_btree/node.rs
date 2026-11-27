// SPDX-License-Identifier: GPL-3.0-or-later
//! One node's bytes: keys packed from the start of the key area and values packed back from the end, both in
//! insertion order, under a table of contents of the given size.

use switch_apfs_format::btree::{
    BTNODE_FIXED_KV_SIZE, BTNODE_HASHED, BTNODE_LEAF, BTNODE_NOHEADER, BTNODE_ROOT, BTOFF_INVALID, BTREE_INFO_SIZE,
    BTREE_NODE_DATA_AT, BtreeInfo, BtreeNodePhys, Kvloc, Kvoff, Nloc,
};
use switch_apfs_format::object::{ObjPhys, TYPE_BTREE, TYPE_BTREE_NODE};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U32};

use crate::TreeShape;

pub(crate) struct NodeSpec<'a> {
    pub shape: &'a TreeShape,
    pub oid: u64,
    pub xid: u64,
    pub level: u16,
    pub toc_entries: usize,
    /// Live entries, then the hole if there is one; a `None` value is a ghost.
    pub entries: &'a [(&'a [u8], Option<&'a [u8]>)],
    pub hole: bool,
    /// Only the root has one.
    pub info: Option<BtreeInfo>,
}

pub(crate) fn write(spec: &NodeSpec) -> Box<Block> {
    let shape = spec.shape;
    let fixed = shape.fixed.is_some();
    let entry_size = if fixed { size_of::<Kvoff>() } else { size_of::<Kvloc>() };
    let values_end = BLOCK_SIZE - if spec.info.is_some() { BTREE_INFO_SIZE } else { 0 };
    let key_area = BTREE_NODE_DATA_AT + spec.toc_entries * entry_size;

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    let mut key_at = 0usize;
    let mut value_back = 0usize;
    let mut holes = (Nloc::EMPTY_LIST, Nloc::EMPTY_LIST);
    let last = spec.entries.len().saturating_sub(1);
    for (i, &(key, value)) in spec.entries.iter().enumerate() {
        block[key_area + key_at..key_area + key_at + key.len()].copy_from_slice(key);
        let value_off = match value {
            Some(value) => {
                value_back += value.len();
                let at = values_end - value_back;
                block[at..at + value.len()].copy_from_slice(value);
                value_back as u16
            }
            None => BTOFF_INVALID,
        };
        let toc_at = BTREE_NODE_DATA_AT + i * entry_size;
        if fixed {
            let entry = Kvoff {
                k: U16::new(key_at as u16),
                v: U16::new(value_off),
            };
            put(&mut block[..], toc_at, &entry);
        } else {
            let entry = Kvloc {
                k: Nloc::new(key_at as u16, key.len() as u16),
                v: Nloc::new(value_off, value.map_or(0, <[u8]>::len) as u16),
            };
            put(&mut block[..], toc_at, &entry);
        }
        if spec.hole && i == last {
            holes = hole(
                &mut block[..],
                key_area,
                key_at,
                key.len(),
                values_end,
                value_off,
                value,
            );
        }
        key_at += key.len();
    }

    let nkeys = spec.entries.len() - usize::from(spec.hole);
    let mut flags = if spec.level == 0 { BTNODE_LEAF } else { 0 };
    if spec.info.is_some() {
        flags |= BTNODE_ROOT;
    }
    if fixed {
        flags |= BTNODE_FIXED_KV_SIZE;
    }
    if shape.hashed() {
        flags |= BTNODE_HASHED;
    }
    if shape.headerless() {
        flags |= BTNODE_NOHEADER;
    }
    let kind = if spec.info.is_some() {
        TYPE_BTREE
    } else {
        TYPE_BTREE_NODE
    };
    let o = if shape.headerless() {
        ObjPhys::default()
    } else {
        ObjPhys::new(spec.oid, spec.xid, shape.storage | kind, shape.subtype)
    };
    let node = BtreeNodePhys {
        o,
        flags: U16::new(flags),
        level: U16::new(spec.level),
        nkeys: U32::new(nkeys as u32),
        table_space: Nloc::new(0, (spec.toc_entries * entry_size) as u16),
        free_space: Nloc::new(key_at as u16, (values_end - value_back - key_area - key_at) as u16),
        key_free_list: holes.0,
        val_free_list: holes.1,
    };
    put(&mut block[..], 0, &node);
    if let Some(info) = &spec.info {
        put(&mut block[..], values_end, info);
    }
    if !shape.headerless() {
        seal(&mut block[..]);
    }
    block
}

/// A free-list entry is an `nloc_t` (next entry, its own length) over the first bytes of the freed space; the rest
/// of the moved entry's bytes are left as they were.
fn hole(
    block: &mut [u8],
    key_area: usize,
    key_off: usize,
    key_len: usize,
    values_end: usize,
    value_off: u16,
    value: Option<&[u8]>,
) -> (Nloc, Nloc) {
    let key_entry = Nloc::new(BTOFF_INVALID, key_len as u16);
    let at = key_area + key_off;
    block[at..at + size_of::<Nloc>()].copy_from_slice(key_entry.as_bytes());
    let key_list = Nloc::new(key_off as u16, key_len as u16);
    let Some(value) = value else {
        return (key_list, Nloc::EMPTY_LIST);
    };
    let value_entry = Nloc::new(BTOFF_INVALID, value.len() as u16);
    let at = values_end - usize::from(value_off);
    block[at..at + size_of::<Nloc>()].copy_from_slice(value_entry.as_bytes());
    (key_list, Nloc::new(value_off, value.len() as u16))
}
