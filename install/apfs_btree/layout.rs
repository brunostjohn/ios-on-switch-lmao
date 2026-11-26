// SPDX-License-Identifier: GPL-3.0-or-later
//! Where each record and child goes when the records are inserted in key order, replaying Apple's sequential insert:
//!
//! - Index nodes split preemptively, from the root down: the split moves only the node's last entry, into a new node
//!   to the right, and that entry's bytes stay behind in the old node as free-list holes. A node is short of room when
//!   its free space, less the table of contents' next growth if the table is full, is below `2 × (K + index value)`,
//!   K being the key length of the record going in.
//! - The kext, which builds every tree but a sealed one, checks the path only when a new leaf is needed, and splits
//!   each node short of room that the new leaf's entry would reach.
//! - The sealer checks every index node on the path before each record of its hashed fs tree, and allows one toc
//!   entry less: `free + toc entry < 2 × (K + index value)`.
//! - A root split keeps the root's oid: the root's entries go to a new compact node on the left, the last one to a new
//!   node on the right (allocated first), and the root takes one entry for each, a level up.
//! - A leaf fills until the next record doesn't fit; that record starts a new leaf to its right.
//!
//! Nodes are numbered in the order Apple allocates them. The kext's rule fits every kext tree seen. The sealer's fits
//! most of its trees but not all, so a rebuilt sealed tree can differ from Apple's in where index nodes split.

use switch_apfs_format::btree::{BTREE_INFO_SIZE, BTREE_NODE_DATA_AT, BTREE_TOC_ENTRY_INCREMENT, Kvloc, Kvoff};
use switch_apfs_format::object::BLOCK_SIZE;

use crate::{BuildError, Record, TreeShape, check_records};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Slot {
    Record(usize),
    Child { key: usize, node: usize },
}

impl Slot {
    /// The record whose key this entry carries: an index entry carries its child's first key.
    pub(crate) fn key_record(self) -> usize {
        match self {
            Slot::Record(r) | Slot::Child { key: r, .. } => r,
        }
    }
}

#[derive(Debug)]
pub(crate) struct Planned {
    pub level: u16,
    pub entries: Vec<Slot>,
    /// The last entry, moved to a right sibling; its bytes stay here on the free lists.
    pub hole: Option<Slot>,
}

pub struct Layout<'r> {
    pub(crate) shape: TreeShape,
    pub(crate) records: &'r [Record],
    /// In allocation order; the root is always the first.
    pub(crate) nodes: Vec<Planned>,
}

impl<'r> Layout<'r> {
    pub fn new(shape: &TreeShape, records: &'r [Record]) -> Result<Self, BuildError> {
        check_records(shape, records)?;
        let mut layout = Layout {
            shape: *shape,
            records,
            nodes: vec![Planned {
                level: 0,
                entries: Vec::new(),
                hole: None,
            }],
        };
        let mut path = vec![0];
        for r in 0..records.len() {
            layout.push(&mut path, r)?;
        }
        Ok(layout)
    }

    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    pub fn height(&self) -> u16 {
        self.nodes[0].level + 1
    }

    fn push(&mut self, path: &mut Vec<usize>, r: usize) -> Result<(), BuildError> {
        let key_len = self.records[r].0.len();
        if self.shape.hashed() {
            let mut depth = 0;
            while depth + 1 < path.len() {
                if self.must_split(path[depth], key_len) {
                    self.split_index(path, depth)?;
                }
                depth += 1;
            }
        }
        let leaf = path[path.len() - 1];
        if self.fits(leaf, Slot::Record(r)) {
            self.nodes[leaf].entries.push(Slot::Record(r));
            return Ok(());
        }
        if self.nodes[leaf].entries.is_empty() {
            return Err(BuildError::TooBig(r));
        }
        if !self.shape.hashed() {
            let reached: Vec<usize> = path[..path.len() - 1]
                .iter()
                .rev()
                .copied()
                .take_while(|&node| self.must_split(node, key_len))
                .collect();
            for node in reached.into_iter().rev() {
                let depth = path.iter().position(|&n| n == node).expect("a node on the path");
                self.split_index(path, depth)?;
            }
        }
        self.start_right(path, path.len() - 1, Slot::Record(r))
    }

    fn must_split(&self, node: usize, key_len: usize) -> bool {
        let n = &self.nodes[node];
        if n.entries.len() < 2 {
            return false;
        }
        let count = n.entries.len() + usize::from(n.hole.is_some());
        let mut free = self.free(node);
        if self.shape.fixed.is_none() && self.toc_entries(node, count) == count {
            free = free.saturating_sub(BTREE_TOC_ENTRY_INCREMENT * self.toc_entry_size());
        }
        let slack = if self.shape.hashed() { self.toc_entry_size() } else { 0 };
        free + slack < 2 * (key_len + self.shape.index_value_len())
    }

    fn split_index(&mut self, path: &mut Vec<usize>, depth: usize) -> Result<(), BuildError> {
        if depth == 0 {
            self.split_root(path, None);
            return Ok(());
        }
        let node = path[depth];
        debug_assert!(self.nodes[node].hole.is_none(), "a node that split is off the path");
        let moved = self.nodes[node].entries.pop().expect("a splitting node has entries");
        self.nodes[node].hole = Some(moved);
        let right = self.alloc(self.nodes[node].level, vec![moved]);
        path[depth] = right;
        self.insert_index(path, depth - 1, moved.key_record(), right)
    }

    /// `slot` doesn't fit the node at `depth`: it starts a new node to the right.
    fn start_right(&mut self, path: &mut Vec<usize>, depth: usize, slot: Slot) -> Result<(), BuildError> {
        if depth == 0 {
            self.split_root(path, Some(slot));
            return Ok(());
        }
        let right = self.alloc(self.nodes[path[depth]].level, vec![slot]);
        path[depth] = right;
        self.insert_index(path, depth - 1, slot.key_record(), right)
    }

    fn insert_index(
        &mut self,
        path: &mut Vec<usize>,
        depth: usize,
        key: usize,
        child: usize,
    ) -> Result<(), BuildError> {
        let slot = Slot::Child { key, node: child };
        if !self.fits(path[depth], slot) {
            return self.start_right(path, depth, slot);
        }
        self.nodes[path[depth]].entries.push(slot);
        Ok(())
    }

    /// The root's entries move down a level: all but `extra` (or, without one, all but the root's last entry) to a
    /// new left node, and that one to a new right node.
    fn split_root(&mut self, path: &mut Vec<usize>, extra: Option<Slot>) {
        let level = self.nodes[0].level;
        let mut entries = std::mem::take(&mut self.nodes[0].entries);
        let moved = extra.unwrap_or_else(|| entries.pop().expect("a full root has entries"));
        let right = self.alloc(level, vec![moved]);
        let left_key = entries[0].key_record();
        let left = self.alloc(level, entries);
        self.nodes[0].level = level + 1;
        self.nodes[0].entries = vec![
            Slot::Child {
                key: left_key,
                node: left,
            },
            Slot::Child {
                key: moved.key_record(),
                node: right,
            },
        ];
        path.insert(1, right);
    }

    fn alloc(&mut self, level: u16, entries: Vec<Slot>) -> usize {
        self.nodes.push(Planned {
            level,
            entries,
            hole: None,
        });
        self.nodes.len() - 1
    }

    fn fits(&self, node: usize, slot: Slot) -> bool {
        let n = &self.nodes[node];
        let count = n.entries.len() + usize::from(n.hole.is_some()) + 1;
        let toc = self.toc_entries(node, count);
        if count > toc {
            return false;
        }
        toc * self.toc_entry_size() + self.used_bytes(node) + self.entry_bytes(n.level, slot) <= self.room(node)
    }

    fn free(&self, node: usize) -> usize {
        let n = &self.nodes[node];
        let count = n.entries.len() + usize::from(n.hole.is_some());
        self.room(node) - self.toc_entries(node, count) * self.toc_entry_size() - self.used_bytes(node)
    }

    fn used_bytes(&self, node: usize) -> usize {
        let n = &self.nodes[node];
        n.entries
            .iter()
            .chain(&n.hole)
            .map(|&s| self.entry_bytes(n.level, s))
            .sum()
    }

    pub(crate) fn entry_bytes(&self, level: u16, slot: Slot) -> usize {
        self.key_len(slot) + self.value_len(level, slot)
    }

    pub(crate) fn key_len(&self, slot: Slot) -> usize {
        self.records[slot.key_record()].0.len()
    }

    pub(crate) fn value_len(&self, level: u16, slot: Slot) -> usize {
        match slot {
            Slot::Record(r) => self.records[r].1.as_ref().map_or(0, Vec::len),
            Slot::Child { .. } => {
                debug_assert!(level > 0);
                self.shape.index_value_len()
            }
        }
    }

    pub(crate) fn room(&self, node: usize) -> usize {
        BLOCK_SIZE - BTREE_NODE_DATA_AT - if node == 0 { BTREE_INFO_SIZE } else { 0 }
    }

    pub(crate) fn toc_entry_size(&self) -> usize {
        if self.shape.fixed.is_some() {
            size_of::<Kvoff>()
        } else {
            size_of::<Kvloc>()
        }
    }

    /// A fixed-size node's table has room for as many entries as the node could hold; a variable-size node's grows
    /// with its entries. Both come in whole increments of 8 entries, and never fewer than 8.
    pub(crate) fn toc_entries(&self, node: usize, count: usize) -> usize {
        let wanted = match self.shape.fixed {
            Some((k, v)) => {
                let value = if self.nodes[node].level == 0 {
                    usize::from(v)
                } else {
                    self.shape.index_value_len()
                };
                self.room(node) / (usize::from(k) + value + size_of::<Kvoff>())
            }
            None => count,
        };
        wanted.max(1).div_ceil(BTREE_TOC_ENTRY_INCREMENT) * BTREE_TOC_ENTRY_INCREMENT
    }
}
