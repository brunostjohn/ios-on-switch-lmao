// SPDX-License-Identifier: GPL-3.0-or-later
use sha2::{Digest, Sha256};
use switch_apfs_format::Block;
use switch_apfs_format::btree::BtreeInfo;
use switch_apfs_format::object::BLOCK_SIZE;
use zerocopy::little_endian::{U32, U64};

use crate::SHA256_SIZE;
use crate::layout::{Layout, Slot};
use crate::node::{self, NodeSpec};

pub struct BuiltNode {
    /// The node's oid: its block address in a physical tree, its virtual oid otherwise.
    pub id: u64,
    pub level: u16,
    pub block: Box<Block>,
}

pub struct Built {
    /// In allocation order, root first.
    pub nodes: Vec<BuiltNode>,
    /// SHA-256 of the root node, for a hashed tree: the integrity object's root hash.
    pub root_hash: Option<[u8; SHA256_SIZE]>,
}

impl Layout<'_> {
    /// Takes one id per node from `ids`, root first, in the order Apple allocates them; a sealed tree's ids must run
    /// upwards from the root's, which its index values count from.
    pub fn emit(&self, xid: u64, ids: &mut dyn FnMut() -> u64) -> Built {
        let ids: Vec<u64> = self.nodes.iter().map(|_| ids()).collect();
        let mut order: Vec<usize> = (0..self.nodes.len()).collect();
        order.sort_by_key(|&n| self.nodes[n].level);
        let mut blocks: Vec<Option<Box<Block>>> = self.nodes.iter().map(|_| None).collect();
        let mut hashes = vec![[0u8; SHA256_SIZE]; self.nodes.len()];
        for n in order {
            let block = self.write_node(n, xid, &ids, &hashes);
            if self.shape.hashed() {
                hashes[n] = Sha256::digest(&block[..]).into();
            }
            blocks[n] = Some(block);
        }
        let nodes = blocks
            .into_iter()
            .zip(&self.nodes)
            .zip(&ids)
            .map(|((block, planned), &id)| BuiltNode {
                id,
                level: planned.level,
                block: block.expect("every node is written"),
            })
            .collect();
        Built {
            nodes,
            root_hash: self.shape.hashed().then_some(hashes[0]),
        }
    }

    fn write_node(&self, n: usize, xid: u64, ids: &[u64], hashes: &[[u8; SHA256_SIZE]]) -> Box<Block> {
        let planned = &self.nodes[n];
        let slots: Vec<Slot> = planned.entries.iter().chain(&planned.hole).copied().collect();
        let hole_at = planned.hole.map(|_| slots.len() - 1);
        let index_values: Vec<Option<Vec<u8>>> = slots
            .iter()
            .enumerate()
            .map(|(i, slot)| match *slot {
                Slot::Record(_) => None,
                // The moved entry's child was still open when it moved, so its hash was never filled in.
                Slot::Child { node: child, .. } => Some(self.index_value(ids, child, hashes, hole_at == Some(i))),
            })
            .collect();
        let entries: Vec<(&[u8], Option<&[u8]>)> = slots
            .iter()
            .zip(&index_values)
            .map(|(slot, index)| {
                let key = self.records[slot.key_record()].0.as_slice();
                let value = match *slot {
                    Slot::Record(r) => self.records[r].1.as_deref(),
                    Slot::Child { .. } => index.as_deref(),
                };
                (key, value)
            })
            .collect();
        node::write(&NodeSpec {
            shape: &self.shape,
            oid: ids[n],
            xid,
            level: planned.level,
            toc_entries: self.toc_entries(n, slots.len()),
            entries: &entries,
            hole: hole_at.is_some(),
            info: (n == 0).then(|| self.info()),
        })
    }

    fn index_value(&self, ids: &[u64], child: usize, hashes: &[[u8; SHA256_SIZE]], moved: bool) -> Vec<u8> {
        if !self.shape.hashed() {
            return ids[child].to_le_bytes().to_vec();
        }
        let mut value = ids[child].wrapping_sub(ids[0]).to_le_bytes().to_vec();
        value.extend_from_slice(if moved { &[0; SHA256_SIZE] } else { &hashes[child] });
        value
    }

    fn info(&self) -> BtreeInfo {
        let (key_size, val_size) = self.shape.fixed.unwrap_or((0, 0));
        let longest_key = self.records.iter().map(|(k, _)| k.len()).max().unwrap_or(0);
        let longest_val = self.records.iter().filter_map(|(_, v)| v.as_ref().map(Vec::len)).max();
        BtreeInfo {
            flags: U32::new(self.shape.info_flags),
            node_size: U32::new(BLOCK_SIZE as u32),
            key_size: U32::new(key_size.into()),
            val_size: U32::new(val_size.into()),
            longest_key: U32::new(longest_key as u32),
            longest_val: U32::new(longest_val.unwrap_or(0) as u32),
            key_count: U64::new(self.records.len() as u64),
            node_count: U64::new(self.nodes.len() as u64),
        }
    }
}
