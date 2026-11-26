// SPDX-License-Identifier: GPL-3.0-or-later
//! Who reads each stream and the object ids a volume stores for it, and which inodes are whole-file clones.
//!
//! A data stream takes the number of the first inode that reads it, as a clone shares its original's; sibling ids and
//! then xattr stream ids follow the last inode. A stream nothing reads, such as one `replace_content` let go, gets no
//! id and isn't written. Clones share a private id: the inodes reading one data stream, and copied compressed files
//! (which have no data stream) whose source inodes shared one.

use std::collections::BTreeMap;

use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME};

use crate::WriteError;
use crate::tree::{InodeRef, Tree, XattrValue};

pub struct Ids {
    pub data_users: Vec<Vec<InodeRef>>,
    /// Per stream, its object id; None for a stream nothing reads.
    pub stream: Vec<Option<u64>>,
    /// Per stream, the object its extent references name as their owner.
    pub owner: Vec<u64>,
    pub siblings: Vec<Vec<u64>>,
    /// Per inode, the private id it stores.
    pub private: Vec<u64>,
    /// Every group of two or more inodes sharing a private id, each in tree order.
    pub clones: Vec<Vec<InodeRef>>,
    pub next: u64,
}

impl Ids {
    pub fn cloned(&self) -> Vec<bool> {
        let mut out = vec![false; self.private.len()];
        for r in self.clones.iter().flatten() {
            out[r.0 as usize] = true;
        }
        out
    }
}

fn is_compression(name: &str) -> bool {
    name == RESOURCE_FORK_XATTR_NAME || name == DECMPFS_XATTR_NAME
}

pub fn ids(tree: &Tree) -> Result<Ids, WriteError> {
    let n = tree.streams.len();
    let mut data_users = vec![Vec::new(); n];
    let mut xattr_users = vec![Vec::new(); n];
    for r in tree.inode_refs() {
        let inode = tree.inode(r);
        if let Some(s) = inode.data {
            data_users[s.0 as usize].push(r);
        }
        for x in &inode.xattrs {
            if let XattrValue::Stream(s) = x.value {
                xattr_users[s.0 as usize].push((r, is_compression(&x.name)));
            }
        }
    }
    let mut next = tree.next_number();
    let mut siblings = Vec::with_capacity(tree.inode_count());
    for r in tree.inode_refs() {
        let links = tree.inode(r).links.len() as u64;
        siblings.push(if links > 1 {
            let ids = (next..next + links).collect();
            next += links;
            ids
        } else {
            Vec::new()
        });
    }
    let mut stream = vec![None; n];
    let mut owner = vec![0; n];
    for s in 0..n {
        match (&data_users[s][..], &xattr_users[s][..]) {
            ([first, ..], []) => {
                stream[s] = Some(tree.number(*first));
                owner[s] = tree.number(*first);
            }
            ([], [(first, compression)]) => {
                stream[s] = Some(next);
                owner[s] = if *compression { tree.number(*first) } else { next };
                next += 1;
            }
            ([], []) => {}
            ([], users) => {
                return Err(WriteError::Spec(format!(
                    "stream {s} is named by {} xattrs; an xattr stream can't be shared",
                    users.len()
                )));
            }
            (_, _) => return Err(WriteError::Spec(format!("stream {s} is both data and an xattr"))),
        }
    }
    let (private, clones) = private_ids(tree, &data_users);
    Ok(Ids {
        data_users,
        stream,
        owner,
        siblings,
        private,
        clones,
        next,
    })
}

fn private_ids(tree: &Tree, data_users: &[Vec<InodeRef>]) -> (Vec<u64>, Vec<Vec<InodeRef>>) {
    let mut copied: BTreeMap<u64, Vec<InodeRef>> = BTreeMap::new();
    for r in tree.inode_refs() {
        let node = tree.inode(r);
        if let (None, Some(id)) = (node.data, node.stored.as_ref().and_then(|s| s.private_id)) {
            copied.entry(id).or_default().push(r);
        }
    }
    let mut private: Vec<u64> = tree.inode_refs().map(|r| tree.number(r)).collect();
    let mut clones = Vec::new();
    for group in data_users.iter().cloned().chain(copied.into_values()) {
        let Some(&first) = group.first() else {
            continue;
        };
        for r in &group {
            private[r.0 as usize] = tree.number(first);
        }
        if group.len() > 1 {
            clones.push(group);
        }
    }
    (private, clones)
}
