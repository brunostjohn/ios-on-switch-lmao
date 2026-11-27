// SPDX-License-Identifier: GPL-3.0-or-later
//! Every node of a tree as Apple wrote it, headerless (sealed) nodes included, for the byte comparisons.

use std::io;

use switch_apfs_btree::{Record, TreeShape};
use switch_apfs_format::btree::{
    BTNODE_HASHED, BTNODE_LEAF, BTNODE_NOHEADER, BTNODE_ROOT, BTREE_INFO_SIZE, BTREE_NODE_DATA_AT, BtreeInfo,
    BtreeNodePhys,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::{OBJ_STORAGETYPE_MASK, OBJ_VIRTUAL, TYPE_FSTREE};
use switch_apfs_format::{BLOCK_SIZE, Block, get, header};

pub struct AppleNode {
    pub id: u64,
    pub level: u16,
    pub xid: u64,
    pub block: Box<Block>,
    /// Bytes on its key or value free list.
    pub holes: u16,
}

pub struct AppleTree {
    pub shape: TreeShape,
    /// Depth first, root first.
    pub nodes: Vec<AppleNode>,
    pub records: Vec<Record>,
}

fn invalid(what: String) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what)
}

/// The tree rooted at `root`; `resolve` turns a node's oid into its block address.
pub fn read(src: &dyn BlockSource, root: u64, resolve: &dyn Fn(u64) -> io::Result<u64>) -> io::Result<AppleTree> {
    let block = src.block(resolve(root)?)?;
    let info: BtreeInfo = get(&block[..], BLOCK_SIZE - BTREE_INFO_SIZE).ok_or_else(|| invalid("short root".into()))?;
    let node: BtreeNodePhys = get(&block[..], 0).ok_or_else(|| invalid("short root".into()))?;
    let o = header(&block[..]);
    let (storage, subtype) = if node.flags.get() & BTNODE_NOHEADER != 0 {
        (OBJ_VIRTUAL, TYPE_FSTREE)
    } else {
        (o.type_and_flags.get() & OBJ_STORAGETYPE_MASK, o.subtype.get())
    };
    let fixed = (info.key_size.get() != 0).then(|| (info.key_size.get() as u16, info.val_size.get() as u16));
    let shape = TreeShape {
        storage,
        subtype,
        info_flags: info.flags.get(),
        fixed,
    };
    let mut tree = AppleTree {
        shape,
        nodes: Vec::new(),
        records: Vec::new(),
    };
    let mut stack = vec![root];
    while let Some(id) = stack.pop() {
        let block = src.block(resolve(id)?)?;
        let n: BtreeNodePhys = get(&block[..], 0).ok_or_else(|| invalid(format!("node {id}")))?;
        let entries = entries(&shape, &block[..], &n);
        if n.flags.get() & BTNODE_LEAF != 0 {
            tree.records.extend(entries);
        } else {
            for (_, value) in entries.into_iter().rev() {
                let value = value.ok_or_else(|| invalid(format!("index node {id} has a ghost")))?;
                let child = u64::from_le_bytes(value[..8].try_into().unwrap_or_default());
                stack.push(if n.flags.get() & BTNODE_HASHED != 0 {
                    root + child
                } else {
                    child
                });
            }
        }
        tree.nodes.push(AppleNode {
            id,
            level: n.level.get(),
            xid: header(&block[..]).xid.get(),
            holes: n.key_free_list.len.get() + n.val_free_list.len.get(),
            block,
        });
    }
    Ok(tree)
}

fn entries(shape: &TreeShape, block: &[u8], n: &BtreeNodePhys) -> Vec<Record> {
    let flags = n.flags.get();
    let toc = BTREE_NODE_DATA_AT + usize::from(n.table_space.off.get());
    let keys = toc + usize::from(n.table_space.len.get());
    let values_end = BLOCK_SIZE - if flags & BTNODE_ROOT != 0 { BTREE_INFO_SIZE } else { 0 };
    let u16_at = |at: usize| usize::from(u16::from_le_bytes([block[at], block[at + 1]]));
    let mut out = Vec::new();
    for i in 0..n.nkeys.get() as usize {
        let (k, kl, v, vl) = match shape.fixed {
            Some((ks, vs)) => {
                let vl = if flags & BTNODE_LEAF != 0 {
                    usize::from(vs)
                } else {
                    shape.index_value_len()
                };
                (u16_at(toc + 4 * i), usize::from(ks), u16_at(toc + 4 * i + 2), vl)
            }
            None => (
                u16_at(toc + 8 * i),
                u16_at(toc + 8 * i + 2),
                u16_at(toc + 8 * i + 4),
                u16_at(toc + 8 * i + 6),
            ),
        };
        let key = block[keys + k..keys + k + kl].to_vec();
        let value = (v != 0xFFFF).then(|| block[values_end - v..values_end - v + vl].to_vec());
        out.push((key, value));
    }
    out
}
