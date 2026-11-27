// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
// Forked from dpp's apfs 0.4.1 src/btree.rs (BTreeNode), MIT, Copyright (c) 2026 Dil4rd; see LICENSE-dpp.
// Changed: zerocopy layouts, the 56-byte node header, ghost values, bounds checks that return errors, and hashed
// (sealed) index entries.

use switch_apfs_format::btree::{
    BTNODE_FIXED_KV_SIZE, BTNODE_HASHED, BTNODE_LEAF, BTNODE_NOHEADER, BTNODE_ROOT, BTOFF_INVALID, BTREE_INFO_SIZE,
    BTREE_NODE_DATA_AT, BtreeInfo, BtreeNodePhys, INDEX_VALUE_SIZE, Kvloc, Kvoff,
};
use switch_apfs_format::fs_records::{APFS_HASH_SHA256_SIZE, HASHED_INDEX_VALUE_SIZE};
use switch_apfs_format::get;

use crate::ReadError;

/// Key and value sizes of a fixed-size tree, from its root's `btree_info_t`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fixed {
    pub key: usize,
    pub value: usize,
}

pub struct Node<'a> {
    block: &'a [u8],
    pub header: BtreeNodePhys,
    pub info: Option<BtreeInfo>,
    fixed: Option<Fixed>,
    toc_at: usize,
    key_area: usize,
    values_end: usize,
}

impl<'a> Node<'a> {
    /// `fixed` comes from the root; a root passes `None` and its own `btree_info_t` supplies it.
    pub fn parse(block: &'a [u8], fixed: Option<Fixed>) -> Result<Self, ReadError> {
        let header: BtreeNodePhys = get(block, 0).ok_or_else(|| ReadError::invalid("short B-tree node"))?;
        let flags = header.flags.get();
        let info = if flags & BTNODE_ROOT != 0 {
            get::<BtreeInfo>(block, block.len() - BTREE_INFO_SIZE)
        } else {
            None
        };
        let fixed = match (flags & BTNODE_FIXED_KV_SIZE != 0, fixed, &info) {
            (false, _, _) => None,
            (true, Some(f), _) => Some(f),
            (true, None, Some(info)) => Some(Fixed {
                key: info.key_size.get() as usize,
                value: info.val_size.get() as usize,
            }),
            (true, None, None) => return Err(ReadError::invalid("fixed-size node without its tree's sizes")),
        };
        let toc_at = BTREE_NODE_DATA_AT + usize::from(header.table_space.off.get());
        let key_area = toc_at + usize::from(header.table_space.len.get());
        let values_end = block.len() - if info.is_some() { BTREE_INFO_SIZE } else { 0 };
        let entry = if fixed.is_some() {
            size_of::<Kvoff>()
        } else {
            size_of::<Kvloc>()
        };
        if key_area > values_end || toc_at + header.nkeys.get() as usize * entry > key_area {
            return Err(ReadError::invalid("B-tree node table of contents out of bounds"));
        }
        Ok(Self {
            block,
            header,
            info,
            fixed,
            toc_at,
            key_area,
            values_end,
        })
    }

    pub fn len(&self) -> usize {
        self.header.nkeys.get() as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn is_leaf(&self) -> bool {
        self.header.flags.get() & BTNODE_LEAF != 0
    }

    pub fn level(&self) -> u16 {
        self.header.level.get()
    }

    pub fn fixed(&self) -> Option<Fixed> {
        self.fixed
    }

    pub fn is_hashed(&self) -> bool {
        self.header.flags.get() & BTNODE_HASHED != 0
    }

    pub fn is_headerless(&self) -> bool {
        self.header.flags.get() & BTNODE_NOHEADER != 0
    }

    fn index_value_size(&self) -> usize {
        if self.is_hashed() {
            HASHED_INDEX_VALUE_SIZE
        } else {
            INDEX_VALUE_SIZE
        }
    }

    fn locate(&self, i: usize) -> Result<(usize, usize, u16, usize), ReadError> {
        if i >= self.len() {
            return Err(ReadError::invalid("B-tree entry past nkeys"));
        }
        Ok(match self.fixed {
            Some(f) => {
                let e: Kvoff = get(self.block, self.toc_at + i * size_of::<Kvoff>()).unwrap_or_default();
                let value_len = if self.is_leaf() {
                    f.value
                } else {
                    self.index_value_size()
                };
                (usize::from(e.k.get()), f.key, e.v.get(), value_len)
            }
            None => {
                let e: Kvloc = get(self.block, self.toc_at + i * size_of::<Kvloc>()).unwrap_or_default();
                (
                    usize::from(e.k.off.get()),
                    usize::from(e.k.len.get()),
                    e.v.off.get(),
                    usize::from(e.v.len.get()),
                )
            }
        })
    }

    pub fn key(&self, i: usize) -> Result<&'a [u8], ReadError> {
        let (off, len, _, _) = self.locate(i)?;
        let start = self.key_area + off;
        self.block
            .get(start..start + len)
            .filter(|_| start + len <= self.values_end)
            .ok_or_else(|| ReadError::invalid(format!("key {i} out of bounds")))
    }

    /// `None` for a ghost entry, which has a key and no value.
    pub fn value(&self, i: usize) -> Result<Option<&'a [u8]>, ReadError> {
        let (_, _, off, len) = self.locate(i)?;
        if off == BTOFF_INVALID {
            return Ok(None);
        }
        let off = usize::from(off);
        if off > self.values_end - self.key_area || len > off {
            return Err(ReadError::invalid(format!("value {i} out of bounds")));
        }
        Ok(Some(&self.block[self.values_end - off..self.values_end - off + len]))
    }

    /// The child's oid as stored. In a hashed tree it is relative to the root's oid.
    pub fn child(&self, i: usize) -> Result<u64, ReadError> {
        let value = self.index_value(i)?;
        Ok(u64::from_le_bytes(value[..8].try_into().unwrap_or_default()))
    }

    /// The SHA-256 of the child's whole node, in a hashed tree.
    pub fn child_hash(&self, i: usize) -> Result<Option<&'a [u8]>, ReadError> {
        if !self.is_hashed() {
            return Ok(None);
        }
        Ok(Some(&self.index_value(i)?[8..8 + APFS_HASH_SHA256_SIZE]))
    }

    fn index_value(&self, i: usize) -> Result<&'a [u8], ReadError> {
        let value = self
            .value(i)?
            .ok_or_else(|| ReadError::invalid("index entry without a child"))?;
        if self.is_leaf() || value.len() != self.index_value_size() {
            return Err(ReadError::invalid(format!(
                "index entry {i}: {} bytes, {} expected",
                value.len(),
                self.index_value_size()
            )));
        }
        Ok(value)
    }
}
