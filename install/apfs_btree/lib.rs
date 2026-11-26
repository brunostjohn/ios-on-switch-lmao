// SPDX-License-Identifier: GPL-3.0-or-later
//! B-tree nodes laid out as Apple's kext lays out a node it filled in key order: keys packed from the start of the
//! key area, values packed back from the end, and a table of contents sized the way Apple's nodes are.

use switch_apfs_format::btree::{
    BTNODE_FIXED_KV_SIZE, BTNODE_LEAF, BTNODE_ROOT, BTOFF_INVALID, BTREE_ALLOW_GHOSTS, BTREE_EPHEMERAL,
    BTREE_INFO_SIZE, BTREE_KV_NONALIGNED, BTREE_NODE_DATA_AT, BTREE_PHYSICAL, BTREE_SEQUENTIAL_INSERT,
    BTREE_TOC_ENTRY_INCREMENT, BtreeInfo, BtreeNodePhys, Kvloc, Kvoff, Nloc,
};
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BLOCKREFTREE, TYPE_BTREE, TYPE_FSTREE, TYPE_OMAP,
    TYPE_SNAPMETATREE, TYPE_SPACEMAN_FREE_QUEUE,
};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal};
use zerocopy::little_endian::{U16, U32, U64};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TreeShape {
    pub storage: u32,
    pub subtype: u32,
    pub info_flags: u32,
    pub fixed: Option<(u16, u16)>,
}

impl TreeShape {
    pub const OMAP: TreeShape = TreeShape {
        storage: OBJ_PHYSICAL,
        subtype: TYPE_OMAP,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL,
        fixed: Some((16, 16)),
    };
    pub const FREE_QUEUE: TreeShape = TreeShape {
        storage: OBJ_EPHEMERAL,
        subtype: TYPE_SPACEMAN_FREE_QUEUE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_ALLOW_GHOSTS | BTREE_EPHEMERAL,
        fixed: Some((16, 8)),
    };
    pub const FS_TREE: TreeShape = TreeShape {
        storage: OBJ_VIRTUAL,
        subtype: TYPE_FSTREE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_KV_NONALIGNED,
        fixed: None,
    };
    pub const EXTENT_REF: TreeShape = TreeShape {
        storage: OBJ_PHYSICAL,
        subtype: TYPE_BLOCKREFTREE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED,
        fixed: None,
    };
    pub const SNAP_META: TreeShape = TreeShape {
        storage: OBJ_PHYSICAL,
        subtype: TYPE_SNAPMETATREE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL | BTREE_KV_NONALIGNED,
        fixed: None,
    };
}

/// A leaf record; a `None` value is a ghost, which only a tree with `BTREE_ALLOW_GHOSTS` may hold.
pub type Record = (Vec<u8>, Option<Vec<u8>>);

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum BuildError {
    #[error("{records} records ({bytes} bytes) don't fit one root leaf")]
    Overflow { records: usize, bytes: usize },
    #[error("record {0} doesn't have the tree's fixed key or value size")]
    WrongSize(usize),
    #[error("record {0} is a ghost in a tree that doesn't allow them")]
    Ghost(usize),
    #[error("a variable-size tree without BTREE_KV_NONALIGNED needs aligned keys and values")]
    Aligned,
}

/// One node that is both the root and the only leaf. Records must already be in the tree's key order.
pub fn leaf_root(shape: &TreeShape, oid: u64, xid: u64, records: &[Record]) -> Result<Box<Block>, BuildError> {
    if shape.fixed.is_none() && shape.info_flags & BTREE_KV_NONALIGNED == 0 {
        return Err(BuildError::Aligned);
    }
    check_records(shape, records)?;
    let values_end = BLOCK_SIZE - BTREE_INFO_SIZE;
    let toc_entries = toc_entries(shape, records.len(), values_end);
    let entry_size = if shape.fixed.is_some() {
        size_of::<Kvoff>()
    } else {
        size_of::<Kvloc>()
    };
    let key_area = BTREE_NODE_DATA_AT + toc_entries * entry_size;
    let key_bytes: usize = records.iter().map(|(k, _)| k.len()).sum();
    let value_bytes: usize = records.iter().filter_map(|(_, v)| v.as_ref()).map(Vec::len).sum();
    if records.len() > toc_entries || key_area + key_bytes + value_bytes > values_end {
        return Err(BuildError::Overflow {
            records: records.len(),
            bytes: key_bytes + value_bytes,
        });
    }

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    let mut key_at = 0usize;
    let mut value_back = 0usize;
    for (i, (key, value)) in records.iter().enumerate() {
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
        if shape.fixed.is_some() {
            put(
                &mut block[..],
                toc_at,
                &Kvoff {
                    k: U16::new(key_at as u16),
                    v: U16::new(value_off),
                },
            );
        } else {
            let value_len = value.as_ref().map_or(0, Vec::len) as u16;
            let entry = Kvloc {
                k: Nloc::new(key_at as u16, key.len() as u16),
                v: Nloc::new(value_off, value_len),
            };
            put(&mut block[..], toc_at, &entry);
        }
        key_at += key.len();
    }

    let fixed_flag = if shape.fixed.is_some() { BTNODE_FIXED_KV_SIZE } else { 0 };
    let node = BtreeNodePhys {
        o: ObjPhys::new(oid, xid, shape.storage | TYPE_BTREE, shape.subtype),
        flags: U16::new(BTNODE_ROOT | BTNODE_LEAF | fixed_flag),
        level: U16::ZERO,
        nkeys: U32::new(records.len() as u32),
        table_space: Nloc::new(0, (toc_entries * entry_size) as u16),
        free_space: Nloc::new(
            key_bytes as u16,
            (values_end - value_bytes - key_area - key_bytes) as u16,
        ),
        key_free_list: Nloc::EMPTY_LIST,
        val_free_list: Nloc::EMPTY_LIST,
    };
    put(&mut block[..], 0, &node);
    let (key_size, val_size) = shape.fixed.unwrap_or((0, 0));
    let info = BtreeInfo {
        flags: U32::new(shape.info_flags),
        node_size: U32::new(BLOCK_SIZE as u32),
        key_size: U32::new(key_size.into()),
        val_size: U32::new(val_size.into()),
        longest_key: U32::new(records.iter().map(|(k, _)| k.len()).max().unwrap_or(0) as u32),
        longest_val: U32::new(
            records
                .iter()
                .filter_map(|(_, v)| v.as_ref())
                .map(Vec::len)
                .max()
                .unwrap_or(0) as u32,
        ),
        key_count: U64::new(records.len() as u64),
        node_count: U64::new(1),
    };
    put(&mut block[..], values_end, &info);
    seal(&mut block[..]);
    Ok(block)
}

/// Apple sizes a fixed-size node's table for as many entries as the node could hold, and a variable-size node's for
/// its keys; both round up to whole increments of 8 entries.
fn toc_entries(shape: &TreeShape, records: usize, values_end: usize) -> usize {
    let wanted = match shape.fixed {
        Some((k, v)) => (values_end - BTREE_NODE_DATA_AT) / (usize::from(k) + usize::from(v) + size_of::<Kvoff>()),
        None => records.max(1),
    };
    wanted.div_ceil(BTREE_TOC_ENTRY_INCREMENT) * BTREE_TOC_ENTRY_INCREMENT
}

fn check_records(shape: &TreeShape, records: &[Record]) -> Result<(), BuildError> {
    for (i, (key, value)) in records.iter().enumerate() {
        if value.is_none() && shape.info_flags & BTREE_ALLOW_GHOSTS == 0 {
            return Err(BuildError::Ghost(i));
        }
        if let Some((k, v)) = shape.fixed
            && (key.len() != usize::from(k) || value.as_ref().is_some_and(|value| value.len() != usize::from(v)))
        {
            return Err(BuildError::WrongSize(i));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
