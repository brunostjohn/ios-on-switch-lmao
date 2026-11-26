// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
// Forked from dpp's apfs 0.4.1 src/btree.rs (btree_scan_node), MIT, Copyright (c) 2026 Dil4rd; see LICENSE-dpp.
// Changed: every record in order instead of a comparator range, child oids resolved by the caller's storage
// (physical, ephemeral or virtual), node headers checked, and the visited nodes' addresses returned.

use switch_apfs_format::object::{TYPE_BTREE, TYPE_BTREE_NODE};
use switch_apfs_format::{Block, checksum_ok, header};

use crate::ReadError;
use crate::node::{Fixed, Node};

const MAX_DEPTH: u16 = 16;

/// Reads a node by the oid its parent names: a block number for a physical tree, a checkpoint-map or object-map
/// lookup for an ephemeral or virtual one. Returns the node's block address and bytes.
pub trait NodeReader {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError>;
}

pub struct Visited {
    pub nodes: Vec<u64>,
    pub records: usize,
}

/// Every leaf record of the tree rooted at `root`, in key order. A ghost has no value.
pub fn walk(
    reader: &dyn NodeReader,
    root: u64,
    visit: &mut dyn FnMut(&[u8], Option<&[u8]>) -> Result<(), ReadError>,
) -> Result<Visited, ReadError> {
    let mut visited = Visited {
        nodes: Vec::new(),
        records: 0,
    };
    walk_node(reader, root, None, None, &mut visited, visit)?;
    Ok(visited)
}

fn walk_node(
    reader: &dyn NodeReader,
    oid: u64,
    parent_level: Option<u16>,
    fixed: Option<Fixed>,
    visited: &mut Visited,
    visit: &mut dyn FnMut(&[u8], Option<&[u8]>) -> Result<(), ReadError>,
) -> Result<(), ReadError> {
    let (paddr, block) = reader.node(oid)?;
    if !checksum_ok(&block[..]) {
        return Err(ReadError::invalid(format!(
            "B-tree node {oid} at {paddr}: bad checksum"
        )));
    }
    let want = if parent_level.is_none() {
        TYPE_BTREE
    } else {
        TYPE_BTREE_NODE
    };
    if header(&block[..]).object_type() != want {
        return Err(ReadError::invalid(format!(
            "B-tree node {oid} at {paddr}: object type is not {want:#x}"
        )));
    }
    let node = Node::parse(&block[..], fixed)?;
    let expected = parent_level.map(|l| l.wrapping_sub(1));
    if node.level() > MAX_DEPTH || expected.is_some_and(|l| l != node.level()) || node.is_leaf() != (node.level() == 0)
    {
        return Err(ReadError::invalid(format!(
            "B-tree node {oid} at {paddr}: level {} out of place",
            node.level()
        )));
    }
    visited.nodes.push(paddr);
    for i in 0..node.len() {
        if node.is_leaf() {
            visit(node.key(i)?, node.value(i)?)?;
            visited.records += 1;
        } else {
            walk_node(reader, node.child(i)?, Some(node.level()), node.fixed(), visited, visit)?;
        }
    }
    Ok(())
}
