// SPDX-License-Identifier: GPL-3.0-or-later
//! B-trees built from records already in key order, laid out node for node as Apple's B-tree code lays out a tree it
//! fills by sequential insertion: the kext's own extent-reference trees and the sealer's fs and fext trees.
//!
//! `Layout::new` decides every node without addresses, so a caller can count the nodes before it allocates them;
//! `Layout::emit` then numbers the nodes in Apple's allocation order and writes them.

mod emit;
mod layout;
mod node;

pub use emit::{Built, BuiltNode};
pub use layout::Layout;

use switch_apfs_format::Block;
use switch_apfs_format::btree::{
    BTREE_ALLOW_GHOSTS, BTREE_EPHEMERAL, BTREE_HASHED, BTREE_KV_NONALIGNED, BTREE_NOHEADER, BTREE_PHYSICAL,
    BTREE_SEQUENTIAL_INSERT, INDEX_VALUE_SIZE,
};
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, OBJ_VIRTUAL, TYPE_BLOCKREFTREE, TYPE_FSTREE, TYPE_OMAP, TYPE_SNAPMETATREE,
    TYPE_SPACEMAN_FREE_QUEUE,
};

/// `OBJECT_TYPE_FEXT_TREE`, the subtype of a sealed volume's file-extent tree.
const TYPE_FEXT_TREE: u32 = 0x1F;
pub const SHA256_SIZE: usize = 32;

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
    /// A sealed volume's fs tree: no object headers, and each index value is the child's oid less the root's,
    /// then the SHA-256 of the child's whole node.
    pub const SEALED_FS_TREE: TreeShape = TreeShape {
        storage: OBJ_VIRTUAL,
        subtype: TYPE_FSTREE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_KV_NONALIGNED | BTREE_HASHED | BTREE_NOHEADER,
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
    pub const FEXT: TreeShape = TreeShape {
        storage: OBJ_PHYSICAL,
        subtype: TYPE_FEXT_TREE,
        info_flags: BTREE_SEQUENTIAL_INSERT | BTREE_PHYSICAL,
        fixed: Some((16, 16)),
    };

    pub fn hashed(&self) -> bool {
        self.info_flags & BTREE_HASHED != 0
    }

    pub fn headerless(&self) -> bool {
        self.info_flags & BTREE_NOHEADER != 0
    }

    pub fn index_value_len(&self) -> usize {
        INDEX_VALUE_SIZE + if self.hashed() { SHA256_SIZE } else { 0 }
    }
}

/// A leaf record; a `None` value is a ghost, which only a tree with `BTREE_ALLOW_GHOSTS` may hold.
pub type Record = (Vec<u8>, Option<Vec<u8>>);

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum BuildError {
    #[error("{records} records ({bytes} bytes) don't fit one root leaf")]
    Overflow { records: usize, bytes: usize },
    #[error("record {0} doesn't fit an empty node")]
    TooBig(usize),
    #[error("record {0} doesn't have the tree's fixed key or value size")]
    WrongSize(usize),
    #[error("record {0} is a ghost in a tree that doesn't allow them")]
    Ghost(usize),
    #[error("a variable-size tree without BTREE_KV_NONALIGNED needs aligned keys and values")]
    Aligned,
}

/// The whole tree, its nodes numbered by `ids` in Apple's allocation order, root first.
pub fn build(
    shape: &TreeShape,
    xid: u64,
    records: &[Record],
    ids: &mut dyn FnMut() -> u64,
) -> Result<Built, BuildError> {
    Ok(Layout::new(shape, records)?.emit(xid, ids))
}

/// One node that is both the root and the only leaf.
pub fn leaf_root(shape: &TreeShape, oid: u64, xid: u64, records: &[Record]) -> Result<Box<Block>, BuildError> {
    let layout = Layout::new(shape, records)?;
    if layout.node_count() > 1 {
        let bytes = records
            .iter()
            .map(|(k, v)| k.len() + v.as_ref().map_or(0, Vec::len))
            .sum();
        return Err(BuildError::Overflow {
            records: records.len(),
            bytes,
        });
    }
    let mut built = layout.emit(xid, &mut || oid);
    Ok(built.nodes.swap_remove(0).block)
}

fn check_records(shape: &TreeShape, records: &[Record]) -> Result<(), BuildError> {
    if shape.fixed.is_none() && shape.info_flags & BTREE_KV_NONALIGNED == 0 {
        return Err(BuildError::Aligned);
    }
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
#[cfg(test)]
mod tests_multi;
