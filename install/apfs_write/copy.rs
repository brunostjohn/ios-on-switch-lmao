// SPDX-License-Identifier: GPL-3.0-or-later
//! A verbatim copy of a source volume's subtree into a tree: every inode with its attributes, times, flags, xfields
//! and xattrs as stored, the names with their stored hashes, and the data as the source's own extents, which phase C
//! copies block for block. Streams two copied inodes share stay one stream. A hard link whose other names lie
//! outside the subtree keeps only the names inside it.

use std::collections::{BTreeMap, HashMap};
use std::sync::Arc;

use switch_apfs_format::fs::{Dstream, INO_EXT_TYPE_DSTREAM, S_IFDIR, S_IFREG};
use switch_apfs_format::fs_records::{DREC_EXT_TYPE_SIBLING_ID, S_IFLNK, S_IFMT};
use switch_apfs_read::{Entry, Fs, ReadError, Step, Stream as SourceStream, XattrData};

use crate::WriteError;
use crate::content::{Content, SourceExtent};
use crate::tree::{Attrs, InodeRef, Kind, Stored, StoredDstream, StreamRef, Times, Tree, Xattr, XattrValue};

#[derive(Debug, Default)]
pub struct Copied {
    /// Each copied source inode and where it went.
    pub inodes: BTreeMap<u64, InodeRef>,
    pub entries: u64,
    pub streams: u64,
    /// Source data streams more than one copied inode shares.
    pub shared_streams: u64,
    /// Names the source's hard links have outside the subtree, dropped.
    pub names_outside: u64,
}

struct State<'t> {
    tree: &'t mut Tree,
    root_oid: u64,
    under: InodeRef,
    streams: HashMap<u64, StreamRef>,
    primary: HashMap<InodeRef, (u64, String)>,
    nlink: HashMap<InodeRef, u32>,
    out: Copied,
}

/// Copies everything under `path` in `fs` into `under`, whose own attributes are left as they are.
pub fn copy_subtree(fs: &Fs, path: &str, tree: &mut Tree, under: InodeRef) -> Result<Copied, WriteError> {
    let root_oid = fs
        .lookup(path)?
        .ok_or_else(|| WriteError::Refused(format!("{path}: not in the source")))?;
    if fs.seal.is_some() {
        return Err(WriteError::Refused("a sealed source volume".into()));
    }
    let mut state = State {
        tree,
        root_oid,
        under,
        streams: HashMap::new(),
        primary: HashMap::new(),
        nlink: HashMap::new(),
        out: Copied::default(),
    };
    let mut failure = None;
    fs.walk(path, &mut |entry| {
        if entry.drec.is_none() {
            return Ok(Step::Into);
        }
        match state.entry(entry) {
            Ok(()) => Ok(Step::Into),
            Err(e) => {
                let message = e.to_string();
                failure = Some(e);
                Err(ReadError::Invalid(message))
            }
        }
    })
    .map_err(|e| failure.take().unwrap_or(WriteError::Read(e)))?;
    state.finish();
    Ok(state.out)
}

impl State<'_> {
    fn entry(&mut self, e: &Entry) -> Result<(), WriteError> {
        let drec = e.drec.as_ref().ok_or(WriteError::Source("an entry without a name"))?;
        let parent = if drec.parent == self.root_oid {
            self.under
        } else {
            *self
                .out
                .inodes
                .get(&drec.parent)
                .ok_or_else(|| WriteError::Refused(format!("{}: its parent wasn't copied", e.path)))?
        };
        let xfields = drec
            .xfields
            .iter()
            .filter(|x| x.kind != DREC_EXT_TYPE_SIBLING_ID)
            .cloned()
            .collect();
        let name_hash = drec.hash;
        let oid = e.inode.oid;
        self.out.entries += 1;
        if let Some(&inode) = self.out.inodes.get(&oid) {
            self.tree.link(
                inode,
                parent,
                &drec.name,
                name_hash,
                Some(drec.val.date_added.get()),
                xfields,
            )?;
            return Ok(());
        }
        if !e.other.is_empty() {
            let kinds: Vec<u8> = e.other.iter().map(|r| r.header().1).collect();
            return Err(WriteError::Refused(format!(
                "{}: records of types {kinds:?} that the writer doesn't copy",
                e.path
            )));
        }
        let v = &e.inode.val;
        let kind = match v.mode.get() & S_IFMT {
            S_IFDIR => Kind::Dir,
            S_IFREG => Kind::File,
            S_IFLNK => Kind::Symlink,
            other => return Err(WriteError::Refused(format!("{}: file type {other:#o}", e.path))),
        };
        let attrs = Attrs {
            uid: v.owner.get(),
            gid: v.group.get(),
            mode: v.mode.get() & 0o7777,
            bsd_flags: v.bsd_flags.get(),
            times: Some(Times {
                create: v.create_time.get(),
                modify: v.mod_time.get(),
                change: v.change_time.get(),
                access: v.access_time.get(),
            }),
        };
        let stored = Stored {
            private_id: Some(v.private_id.get()),
            internal_flags: v.internal_flags.get(),
            default_protection_class: v.default_protection_class.get(),
            write_generation_counter: v.write_generation_counter.get(),
            uncompressed_size: v.uncompressed_size.get(),
            xfields: e.inode.xfields.clone(),
        };
        let data = match &e.data {
            Some(stream) => {
                let dstream = e
                    .inode
                    .dstream()
                    .ok_or(WriteError::Source("a data stream without a dstream"))?;
                Some(self.stream(stream, stored_dstream(&dstream)))
            }
            None if e.inode.xfield(INO_EXT_TYPE_DSTREAM).is_some() => {
                return Err(WriteError::Source("a dstream xfield the reader didn't decode"));
            }
            None => None,
        };
        let inode = self.tree.push(kind, attrs, data, Some(stored));
        for (x, stream) in &e.xattrs {
            let value = match (&x.data, stream) {
                (XattrData::Embedded(bytes), _) => XattrValue::Embedded(bytes.clone()),
                (XattrData::Stream { dstream, .. }, Some(s)) => {
                    XattrValue::Stream(self.stream(s, stored_dstream(&dstream.dstream)))
                }
                (XattrData::Stream { id, .. }, None) => {
                    return Err(WriteError::Refused(format!("{}: xattr stream {id} unread", e.path)));
                }
            };
            self.tree.inode_mut(inode).xattrs.push(Xattr {
                name: x.name.clone(),
                flags: x.flags,
                value,
            });
        }
        self.out.inodes.insert(oid, inode);
        self.nlink.insert(inode, v.nchildren_or_nlink.get().max(0) as u32);
        if let Some(name) = e.inode.name() {
            self.primary.insert(inode, (v.parent_id.get(), name));
        }
        self.tree.link(
            inode,
            parent,
            &drec.name,
            name_hash,
            Some(drec.val.date_added.get()),
            xfields,
        )?;
        Ok(())
    }

    fn stream(&mut self, s: &SourceStream, stored: StoredDstream) -> StreamRef {
        if let Some(&r) = self.streams.get(&s.id) {
            self.out.shared_streams += 1;
            return r;
        }
        let mut extents: Vec<SourceExtent> = s
            .extents
            .iter()
            .map(|e| SourceExtent {
                logical: e.logical_addr,
                len: e.len,
                flags: e.flags,
                paddr: e.phys_block_num,
                crypto_id: e.crypto_id,
            })
            .collect();
        extents.sort_by_key(|e| e.logical);
        let r = self
            .tree
            .add_stored_stream(s.size, Content::Source(Arc::from(extents)), stored);
        self.streams.insert(s.id, r);
        self.out.streams += 1;
        r
    }

    /// Puts each multi-name inode's source primary name first, when it was copied, and counts the names left behind.
    fn finish(&mut self) {
        for (&inode, &nlink) in &self.nlink {
            let node = self.tree.inode_mut(inode);
            if node.kind == Kind::Dir {
                continue;
            }
            self.out.names_outside += u64::from(nlink).saturating_sub(node.links.len() as u64);
            let Some((parent_oid, name)) = self.primary.get(&inode) else {
                continue;
            };
            let parent = if *parent_oid == self.root_oid {
                Some(self.under)
            } else {
                self.out.inodes.get(parent_oid).copied()
            };
            if let Some(at) = node
                .links
                .iter()
                .position(|l| Some(l.parent) == parent && &l.name == name)
            {
                node.links[..=at].rotate_right(1);
            }
        }
    }
}

fn stored_dstream(d: &Dstream) -> StoredDstream {
    StoredDstream {
        alloced_size: d.alloced_size.get(),
        default_crypto_id: d.default_crypto_id.get(),
        total_bytes_written: d.total_bytes_written.get(),
        total_bytes_read: d.total_bytes_read.get(),
    }
}
