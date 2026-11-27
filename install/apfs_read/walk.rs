// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
// Forked from dpp's apfs 0.4.1 src/btree.rs (btree_scan_node), MIT, Copyright (c) 2026 Dil4rd; see LICENSE-dpp.
// Changed: child oids resolved by the caller's storage (physical, ephemeral or virtual), node headers checked, the
// visited nodes' addresses returned, index subtrees pruned outside the range, and hashed (sealed) trees: children at
// the root's oid plus the stored offset, each node checked against the hash its parent holds.

use std::cmp::Ordering;

use sha2::{Digest, Sha256};
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

pub struct Physical<'a>(pub &'a dyn switch_apfs_format::io::BlockSource);

impl NodeReader for Physical<'_> {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError> {
        Ok((oid, self.0.block(oid)?))
    }
}

pub struct Visited {
    pub nodes: Vec<u64>,
    pub records: usize,
}

pub type Visit<'v> = dyn FnMut(&[u8], Option<&[u8]>) -> Result<(), ReadError> + 'v;

/// Orders a key against the range a scan wants: `Less` before it, `Equal` in it, `Greater` after it. It has to agree
/// with the tree's key order; the scan checks that it does, node by node.
pub type Range<'r> = dyn Fn(&[u8]) -> Result<Ordering, ReadError> + 'r;

pub struct Tree<'a> {
    pub reader: &'a dyn NodeReader,
    pub root: u64,
    /// What the integrity object says the root node hashes to. A hashed tree is refused without it.
    pub root_hash: Option<[u8; 32]>,
}

/// Every leaf record of the tree rooted at `root`, in key order. A ghost has no value.
pub fn walk(reader: &dyn NodeReader, root: u64, visit: &mut Visit) -> Result<Visited, ReadError> {
    scan(
        &Tree {
            reader,
            root,
            root_hash: None,
        },
        None,
        visit,
    )
}

/// The records `range` orders `Equal`, in key order, or every record when there is no range.
pub fn scan(tree: &Tree, range: Option<&Range>, visit: &mut Visit) -> Result<Visited, ReadError> {
    let mut walker = Walker {
        tree,
        range,
        visit,
        visited: Visited {
            nodes: Vec::new(),
            records: 0,
        },
    };
    walker.node(tree.root, tree.root_hash.as_ref().map(|h| &h[..]), None, None)?;
    Ok(walker.visited)
}

struct Walker<'t, 'v> {
    tree: &'t Tree<'t>,
    range: Option<&'t Range<'t>>,
    visit: &'t mut Visit<'v>,
    visited: Visited,
}

enum Flow {
    More,
    Done,
}

impl Walker<'_, '_> {
    fn node(
        &mut self,
        oid: u64,
        want_hash: Option<&[u8]>,
        parent: Option<&Node>,
        fixed: Option<Fixed>,
    ) -> Result<Flow, ReadError> {
        let (paddr, block) = self.tree.reader.node(oid)?;
        let at = || format!("B-tree node {oid} at {paddr}");
        let node = Node::parse(&block[..], fixed).map_err(|e| ReadError::invalid(format!("{}: {e}", at())))?;
        check_header(&block, &node, parent.is_none()).map_err(|e| ReadError::invalid(format!("{}: {e}", at())))?;
        check_hash(&block, &node, parent, want_hash).map_err(|e| ReadError::invalid(format!("{}: {e}", at())))?;
        let expected = parent.map(|p| p.level().wrapping_sub(1));
        if node.level() > MAX_DEPTH
            || expected.is_some_and(|l| l != node.level())
            || node.is_leaf() != (node.level() == 0)
        {
            return Err(ReadError::invalid(format!(
                "{}: level {} out of place",
                at(),
                node.level()
            )));
        }
        self.visited.nodes.push(paddr);
        self.check_order(&node)
            .map_err(|e| ReadError::invalid(format!("{}: {e}", at())))?;
        if node.is_leaf() {
            return self.leaf(&node);
        }
        for i in 0..node.len() {
            if let Some(range) = self.range {
                if range(node.key(i)?)? == Ordering::Greater {
                    return Ok(Flow::Done);
                }
                if i + 1 < node.len() && range(node.key(i + 1)?)? == Ordering::Less {
                    continue;
                }
            }
            let child = if node.is_hashed() {
                self.tree.root.wrapping_add(node.child(i)?)
            } else {
                node.child(i)?
            };
            if let Flow::Done = self.node(child, node.child_hash(i)?, Some(&node), node.fixed())? {
                return Ok(Flow::Done);
            }
        }
        Ok(Flow::More)
    }

    fn leaf(&mut self, node: &Node) -> Result<Flow, ReadError> {
        for i in 0..node.len() {
            let key = node.key(i)?;
            match self.range.map_or(Ok(Ordering::Equal), |r| r(key))? {
                Ordering::Less => continue,
                Ordering::Equal => {
                    (self.visit)(key, node.value(i)?)?;
                    self.visited.records += 1;
                }
                Ordering::Greater => return Ok(Flow::Done),
            }
        }
        Ok(Flow::More)
    }

    /// Keys ascend in a node, so against one range they must order as a run of `Less`, then `Equal`, then
    /// `Greater`. Anything else means the range disagrees with the tree, and a scan would silently miss records.
    fn check_order(&self, node: &Node) -> Result<(), ReadError> {
        let Some(range) = self.range else {
            return Ok(());
        };
        let mut previous = Ordering::Less;
        for i in 0..node.len() {
            let here = range(node.key(i)?)?;
            if here < previous {
                return Err(ReadError::invalid(format!("key {i} is out of order for this scan")));
            }
            previous = here;
        }
        Ok(())
    }
}

fn check_header(block: &Block, node: &Node, root: bool) -> Result<(), String> {
    if node.is_headerless() {
        return if block[..32].iter().all(|&b| b == 0) {
            Ok(())
        } else {
            Err("headerless, with a header".into())
        };
    }
    if !checksum_ok(&block[..]) {
        return Err("bad checksum".into());
    }
    let want = if root { TYPE_BTREE } else { TYPE_BTREE_NODE };
    if header(&block[..]).object_type() != want {
        return Err(format!("object type is not {want:#x}"));
    }
    Ok(())
}

/// Nothing else checks a headerless node, so a hashed tree is checked whole: the root against the integrity
/// object's hash, every other node against its parent's entry.
fn check_hash(block: &Block, node: &Node, parent: Option<&Node>, want: Option<&[u8]>) -> Result<(), String> {
    let hashed = parent.map_or(node.is_hashed(), Node::is_hashed);
    if node.is_hashed() != hashed {
        return Err("hashed in a tree that isn't, or not hashed in one that is".into());
    }
    match (hashed, want) {
        (false, None) => Ok(()),
        (false, Some(_)) => Err("a root hash for a tree that isn't hashed".into()),
        (true, None) => Err("a hashed tree without its root hash".into()),
        (true, Some(want)) if Sha256::digest(&block[..])[..] == *want => Ok(()),
        (true, Some(_)) => Err("SHA-256 doesn't match the hash its parent or the integrity object holds".into()),
    }
}
