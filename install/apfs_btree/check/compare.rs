// SPDX-License-Identifier: GPL-3.0-or-later
//! Rebuilds each of an image's trees from its own leaf records and compares every node with Apple's, byte for byte,
//! and recomputes the hash of every directory entry's name.
//!
//! Only a tree Apple filled by sequential insertion can match; one with leaf free lists had records deleted after
//! they went in. Only the trees named as required must match: the kext fills an fs tree in creation order, and the
//! sealer's index splits are only approximated. A tree the kext wrote over several transactions is compared with each
//! node's xid set to Apple's.

use std::collections::{BTreeSet, HashMap};
use std::io;
use std::path::Path;

use switch_apfs_btree::{Built, Layout};
use switch_apfs_format::fs::APFS_TYPE_DIR_REC;
use switch_apfs_format::io::{BlockSource, FileSource};
use switch_apfs_format::{Block, get, put, seal};
use switch_apfs_names::{UnicodeVersion, name_len_and_hash};
use switch_apfs_read::{Container, Omap};
use zerocopy::little_endian::U64;

use crate::nodes::{self, AppleTree};

const INTEGRITY_ROOT_HASH_OFFSET_AT: usize = 44;
const XID_AT: usize = 16;

enum Verdict {
    Identical,
    /// Identical once each node's xid is the one Apple's node has; Apple wrote the tree over several transactions.
    IdenticalOverTransactions(usize),
    /// Identical given Apple's address for each node as well; later transactions moved nodes they rewrote.
    IdenticalAtApplesAddresses(usize),
    Differs(String),
    NotComparable(String),
}

fn io_err(e: impl std::fmt::Display) -> io::Error {
    io::Error::other(e.to_string())
}

/// Ok(false) when a tree named in `required` isn't identical, or a stored name hash isn't ours. With `only`, just that
/// volume's trees.
pub fn run(image: &Path, only: Option<&str>, required: &[String]) -> io::Result<bool> {
    let src = FileSource::open(image, 0)?;
    let c = Container::open(&src).map_err(io_err)?;
    let mut ok = true;
    let mut identical = BTreeSet::new();
    for v in c.volumes.iter().filter(|v| only.is_none_or(|name| v.sb.name() == name)) {
        let sb = &v.sb;
        let omap = Omap::read(&src, sb.omap_oid.get()).map_err(io_err)?;
        let virt = |oid: u64| {
            omap.lookup(oid, u64::MAX)
                .map(|(_, value)| value.paddr.get())
                .ok_or_else(|| io_err(format!("oid {oid} unmapped")))
        };
        let phys = |paddr: u64| Ok(paddr);
        let mut trees = vec![
            (
                "omap",
                omap.phys.tree_oid.get(),
                &phys as &dyn Fn(u64) -> io::Result<u64>,
            ),
            ("fs", sb.root_tree_oid.get(), &virt),
            ("extentref", sb.extentref_tree_oid.get(), &phys),
        ];
        if sb.fext_tree_oid.get() != 0 {
            trees.push(("fext", sb.fext_tree_oid.get(), &phys));
        }
        for (name, root, resolve) in trees {
            let tree = nodes::read(&src, root, resolve)?;
            let (verdict, root_hash) = rebuild(&tree);
            let levels = tree.nodes[0].level + 1;
            let line = format!(
                "volume {}: {name}: {} nodes, {levels} levels, {} records:",
                sb.name(),
                tree.nodes.len(),
                tree.records.len()
            );
            match verdict {
                Verdict::Identical => {
                    println!("{line} every node byte-identical");
                    identical.insert(name.to_string());
                }
                Verdict::IdenticalOverTransactions(n) => {
                    println!("{line} every node byte-identical with Apple's xid in its header ({n} transactions)");
                    identical.insert(name.to_string());
                }
                Verdict::IdenticalAtApplesAddresses(n) => {
                    println!(
                        "{line} every node byte-identical given Apple's address and xid for it ({n} transactions)"
                    );
                    identical.insert(name.to_string());
                }
                Verdict::Differs(why) if required.iter().any(|r| r == name) => {
                    println!("{line} DIFFERS: {why}");
                    ok = false;
                }
                Verdict::Differs(why) => println!("{line} differs ({why})"),
                Verdict::NotComparable(why) => println!("{line} not comparable ({why})"),
            }
            if name == "fs" {
                ok &= names(&sb.name(), &tree);
                if sb.integrity_meta_oid.get() != 0 {
                    let same = integrity(&src, virt(sb.integrity_meta_oid.get())?, root_hash)?;
                    ok &= same || !required.iter().any(|r| r == "fs");
                }
            }
        }
    }
    for name in required {
        if !identical.contains(name) {
            println!("FAIL: no {name} tree was rebuilt byte for byte");
            ok = false;
        }
    }
    Ok(ok)
}

/// The block address of the last leaf of the named volume's fs tree, for the negative control.
pub fn last_fs_leaf(image: &Path, volume: &str) -> io::Result<u64> {
    let src = FileSource::open(image, 0)?;
    let c = Container::open(&src).map_err(io_err)?;
    let v = c
        .volumes
        .iter()
        .find(|v| v.sb.name() == volume)
        .ok_or_else(|| io_err(format!("no volume {volume}")))?;
    let omap = Omap::read(&src, v.sb.omap_oid.get()).map_err(io_err)?;
    let virt = |oid: u64| {
        omap.lookup(oid, u64::MAX)
            .map(|(_, value)| value.paddr.get())
            .ok_or_else(|| io_err(format!("oid {oid} unmapped")))
    };
    let tree = nodes::read(&src, v.sb.root_tree_oid.get(), &virt)?;
    let leaf = tree
        .nodes
        .iter()
        .rev()
        .find(|n| n.level == 0 && n.id != tree.nodes[0].id);
    virt(leaf.ok_or_else(|| io_err("the fs tree is one node"))?.id)
}

fn rebuild(tree: &AppleTree) -> (Verdict, Option<[u8; 32]>) {
    if tree.nodes.iter().any(|n| n.level == 0 && n.holes != 0) {
        return (Verdict::NotComparable("leaf free lists".into()), None);
    }
    let transactions = tree.nodes.iter().map(|n| n.xid).collect::<BTreeSet<_>>().len();
    let layout = match Layout::new(&tree.shape, &tree.records) {
        Ok(layout) => layout,
        Err(e) => return (Verdict::Differs(e.to_string()), None),
    };
    if layout.node_count() != tree.nodes.len() {
        return (Verdict::Differs(format!("{} nodes rebuilt", layout.node_count())), None);
    }
    let mut ids: Vec<u64> = tree.nodes[1..].iter().map(|n| n.id).collect();
    ids.sort_unstable();
    ids.insert(0, tree.nodes[0].id);
    let built = emit(&layout, tree, &ids);
    let mismatch = match differences(&built, tree, transactions > 1) {
        None if transactions > 1 => return (Verdict::IdenticalOverTransactions(transactions), built.root_hash),
        None => return (Verdict::Identical, built.root_hash),
        Some(why) => why,
    };
    if transactions == 1 {
        return (Verdict::Differs(mismatch), None);
    }
    match apples_addresses(&built, tree).map(|ids| emit(&layout, tree, &ids)) {
        Ok(moved) if differences(&moved, tree, true).is_none() => {
            (Verdict::IdenticalAtApplesAddresses(transactions), None)
        }
        _ => (Verdict::Differs(mismatch), None),
    }
}

fn emit(layout: &Layout, tree: &AppleTree, ids: &[u64]) -> Built {
    let mut next = ids.iter().copied();
    layout.emit(tree.nodes[0].xid, &mut || next.next().unwrap_or(0))
}

/// None when every node equals Apple's node of the same id; with `their_xids`, after taking that node's xid.
fn differences(built: &Built, tree: &AppleTree, their_xids: bool) -> Option<String> {
    let apple: HashMap<u64, &nodes::AppleNode> = tree.nodes.iter().map(|n| (n.id, n)).collect();
    let mut same = 0;
    let mut first = None;
    for node in &built.nodes {
        let theirs = apple.get(&node.id);
        let mut ours = node.block.clone();
        if let Some(t) = theirs
            && their_xids
        {
            put(&mut ours[..], XID_AT, &U64::new(t.xid));
            seal(&mut ours[..]);
        }
        if theirs.is_some_and(|t| t.block == ours) {
            same += 1;
        } else if first.is_none() {
            let at = theirs.and_then(|t| t.block.iter().zip(ours.iter()).position(|(a, b)| a != b));
            first = Some(format!(
                "node {} (level {}) first differs at byte {at:?}",
                node.id, node.level
            ));
        }
    }
    first.map(|why| format!("{same}/{} identical; {why}", built.nodes.len()))
}

/// The id of Apple's node in the same place of the tree as each of ours, in our allocation order. A kext tree written
/// over several transactions has nodes that moved when a later transaction rewrote them.
fn apples_addresses(built: &Built, tree: &AppleTree) -> io::Result<Vec<u64>> {
    let blocks = InMemory(built.nodes.iter().map(|n| (n.id, &*n.block)).collect());
    let ours = nodes::read(&blocks, built.nodes[0].id, &|id| Ok(id))?;
    let index: HashMap<u64, usize> = built.nodes.iter().enumerate().map(|(i, n)| (n.id, i)).collect();
    let mut ids = vec![0; built.nodes.len()];
    for (mine, theirs) in ours.nodes.iter().zip(&tree.nodes) {
        ids[index[&mine.id]] = theirs.id;
    }
    Ok(ids)
}

struct InMemory<'a>(HashMap<u64, &'a Block>);

impl BlockSource for InMemory<'_> {
    fn read_blocks(&self, id: u64, out: &mut [u8]) -> io::Result<()> {
        let block = self.0.get(&id).ok_or_else(|| io_err(format!("no node {id}")))?;
        out.copy_from_slice(&block[..]);
        Ok(())
    }
}

/// Every directory entry's stored `name_len_and_hash` against ours at macOS 27's Unicode 17.0.
fn names(volume: &str, tree: &AppleTree) -> bool {
    let (mut total, mut non_ascii, mut bad) = (0, 0, 0);
    for (key, _) in &tree.records {
        let head = u64::from_le_bytes(key[..8].try_into().unwrap_or_default());
        if (head >> 60) as u8 != APFS_TYPE_DIR_REC {
            continue;
        }
        let stored = u32::from_le_bytes(key[8..12].try_into().unwrap_or_default());
        let len = (stored & 0x3FF) as usize;
        let name = String::from_utf8_lossy(&key[12..12 + len - 1]);
        total += 1;
        non_ascii += usize::from(!name.is_ascii());
        let ours = name_len_and_hash(&name, UnicodeVersion::IOS_27);
        if ours != Ok(stored) {
            bad += 1;
            println!("NAME {name:?}: stored {stored:#010x}, ours {ours:?}");
        }
    }
    println!(
        "volume {volume}: names: {total} entries, {non_ascii} non-ASCII, {} match",
        total - bad
    );
    bad == 0
}

fn integrity(src: &dyn BlockSource, paddr: u64, root_hash: Option<[u8; 32]>) -> io::Result<bool> {
    let block = src.block(paddr)?;
    let at: u32 = get(&block[..], INTEGRITY_ROOT_HASH_OFFSET_AT).unwrap_or_default();
    let stored = &block[at as usize..at as usize + 32];
    let ok = root_hash.is_some_and(|h| h[..] == *stored);
    println!(
        "integrity object: root hash {}",
        if ok { "equals ours" } else { "DIFFERS from ours" }
    );
    Ok(ok)
}
