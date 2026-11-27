// SPDX-License-Identifier: GPL-3.0-or-later
//! Saves the fs-tree nodes a lookup of one object reads, root to leaf, with the root's oid and hash: the unit tests'
//! fixtures for walking a tree from bytes macOS wrote.

use std::cell::RefCell;
use std::path::Path;

use switch_apfs_format::Block;
use switch_apfs_read::walk::{self, NodeReader, Tree};
use switch_apfs_read::{Fs, ReadError, catalog};

struct Recording<'a> {
    fs: &'a Fs<'a>,
    seen: RefCell<Vec<(u64, Box<Block>)>>,
}

impl NodeReader for Recording<'_> {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError> {
        let (paddr, block) = self.fs.node(oid)?;
        self.seen.borrow_mut().push((oid, block.clone()));
        Ok((paddr, block))
    }
}

pub fn capture(fs: &Fs, oid: u64, out: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let rec = Recording {
        fs,
        seen: RefCell::new(Vec::new()),
    };
    let tree = Tree {
        reader: &rec,
        root: fs.sb.root_tree_oid.get(),
        root_hash: fs.seal.map(|s| s.root_hash),
    };
    let range = |key: &[u8]| catalog::order(key, oid, None);
    let mut records = 0;
    walk::scan(&tree, Some(&range), &mut |_, _| {
        records += 1;
        Ok(())
    })?;
    std::fs::create_dir_all(out)?;
    for (i, (node_oid, block)) in rec.seen.borrow().iter().enumerate() {
        std::fs::write(out.join(format!("{i}-{node_oid}.blk")), &block[..])?;
    }
    println!(
        "object {oid}: {records} records; root oid {} hash {}; {} nodes written to {}",
        tree.root,
        tree.root_hash.map(hex::encode).unwrap_or_else(|| "-".into()),
        rec.seen.borrow().len(),
        out.display()
    );
    Ok(())
}
