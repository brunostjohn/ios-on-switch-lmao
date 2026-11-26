// SPDX-License-Identifier: GPL-3.0-or-later
//! A path-level walk of a directory's subtree. Each entry carries what a verbatim copy needs: the inode with every
//! xfield, the directory record, the xattrs with their streams, the hard-link siblings, and the data stream's
//! extents. Nothing is decompressed: decmpfs and resource forks are xattrs like any other.

use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE, APFS_TYPE_XATTR,
};
use switch_apfs_format::fs_records::{APFS_TYPE_FILE_INFO, APFS_TYPE_SIBLING_LINK};

use crate::catalog::{DirRec, FileExtent, FileInfo, Inode, Record, SiblingLink, Xattr, XattrData};
use crate::fs::{Fs, RawRecord};
use crate::{ReadError, invalid};

const STREAM_KINDS: [u8; 3] = [APFS_TYPE_DSTREAM_ID, APFS_TYPE_FILE_EXTENT, APFS_TYPE_FILE_INFO];

/// A data stream: a file's, or a stream xattr's.
#[derive(Clone, Debug)]
pub struct Stream {
    pub id: u64,
    /// Logical size in bytes, from the inode's or the xattr's `j_dstream`.
    pub size: u64,
    pub extents: Vec<FileExtent>,
    /// The seal's hashes of the stream, on a sealed volume.
    pub file_info: Vec<FileInfo>,
    /// From the stream's `DSTREAM_ID` record, when it has one.
    pub refcnt: Option<u32>,
}

#[derive(Clone, Debug)]
pub struct Entry {
    /// `/` for the volume's root, otherwise `/`-separated from it.
    pub path: String,
    /// The record that names this entry in its parent; none for the walk's starting directory.
    pub drec: Option<DirRec>,
    pub inode: Inode,
    pub xattrs: Vec<(Xattr, Option<Stream>)>,
    pub siblings: Vec<SiblingLink>,
    pub data: Option<Stream>,
    /// The object's other records (directory stats and anything not decoded), as stored.
    pub other: Vec<RawRecord>,
}

impl Entry {
    pub fn xattr(&self, name: &str) -> Option<&(Xattr, Option<Stream>)> {
        self.xattrs.iter().find(|(x, _)| x.name == name)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Into,
    Over,
}

impl Fs<'_> {
    /// Visits the entry at `path` and, depth first in on-disk order, everything under it the visitor steps into.
    /// Returns how many entries it visited.
    pub fn walk(&self, path: &str, visit: &mut dyn FnMut(&Entry) -> Result<Step, ReadError>) -> Result<u64, ReadError> {
        let oid = self.lookup(path)?.ok_or_else(|| invalid!("{path}: no such entry"))?;
        let path = match path.trim_matches('/') {
            "" => "/".to_string(),
            p => format!("/{p}"),
        };
        let mut count = 0;
        self.walk_from(oid, path, None, visit, &mut count)?;
        Ok(count)
    }

    fn walk_from(
        &self,
        oid: u64,
        path: String,
        drec: Option<DirRec>,
        visit: &mut dyn FnMut(&Entry) -> Result<Step, ReadError>,
        count: &mut u64,
    ) -> Result<(), ReadError> {
        let (entry, children) = self.entry(oid, path, drec)?;
        *count += 1;
        if visit(&entry)? == Step::Over || !entry.inode.is_dir() {
            return Ok(());
        }
        let base = if entry.path == "/" { "" } else { entry.path.as_str() };
        for child in children {
            let path = format!("{base}/{}", child.name);
            self.walk_from(child.file_id(), path, Some(child), visit, count)?;
        }
        Ok(())
    }

    /// One object as an entry, and its directory records if it is a directory.
    pub fn entry(&self, oid: u64, path: String, drec: Option<DirRec>) -> Result<(Entry, Vec<DirRec>), ReadError> {
        let records = self.object(oid, None)?;
        let at = |e: ReadError| invalid!("{path} (inode {oid}): {e}");
        let mut inode = None;
        let mut xattrs = Vec::new();
        let mut siblings = Vec::new();
        let mut children = Vec::new();
        let mut own_stream = Vec::new();
        let mut other = Vec::new();
        for r in records {
            match r.header().1 {
                APFS_TYPE_INODE if inode.is_none() => inode = Some(Inode::decode(oid, &r.value).map_err(at)?),
                APFS_TYPE_XATTR => xattrs.push(Xattr::decode(&r.key, &r.value).map_err(at)?),
                APFS_TYPE_SIBLING_LINK => siblings.push(SiblingLink::decode(&r.key, &r.value).map_err(at)?),
                APFS_TYPE_DIR_REC => children.push(DirRec::decode(&r.key, &r.value, self.hashed_names()).map_err(at)?),
                kind if STREAM_KINDS.contains(&kind) => own_stream.push(r),
                APFS_TYPE_INODE => return Err(at(invalid!("two inode records"))),
                _ => other.push(r),
            }
        }
        let inode = inode.ok_or_else(|| at(invalid!("no inode record")))?;
        let data = match inode.dstream() {
            Some(d) => {
                let id = inode.val.private_id.get();
                let have = (id == oid).then_some(own_stream.as_slice());
                Some(self.stream(id, d.size.get(), have).map_err(at)?)
            }
            // A truncated clone owner gets a new private_id while a kept clone's stream stays keyed at `oid`.
            None if own_stream.is_empty() || inode.val.private_id.get() != oid => None,
            None => return Err(at(invalid!("stream records without a dstream"))),
        };
        let xattrs = xattrs
            .into_iter()
            .map(|x| {
                let stream = match &x.data {
                    XattrData::Embedded(_) => None,
                    XattrData::Stream { id, dstream } => Some(self.stream(*id, dstream.dstream.size.get(), None)?),
                };
                Ok((x, stream))
            })
            .collect::<Result<Vec<_>, ReadError>>()
            .map_err(at)?;
        let entry = Entry {
            path,
            drec,
            inode,
            xattrs,
            siblings,
            data,
            other,
        };
        Ok((entry, children))
    }

    /// `have` holds the stream's records when they came with its inode's, which they do when the ids are the same.
    fn stream(&self, id: u64, size: u64, have: Option<&[RawRecord]>) -> Result<Stream, ReadError> {
        let fetched;
        let records = match have {
            Some(r) => r,
            None => {
                fetched = self.object(id, None)?;
                fetched.as_slice()
            }
        };
        let mut stream = Stream {
            id,
            size,
            extents: Vec::new(),
            file_info: Vec::new(),
            refcnt: None,
        };
        // A stream's id can also be an inode's, whose records share the range; they aren't the stream's.
        for r in records.iter().filter(|r| STREAM_KINDS.contains(&r.header().1)) {
            match Record::decode(&r.key, &r.value, self.hashed_names())? {
                Record::FileExtent(e) => stream.extents.push(e),
                Record::FileInfo(i) => stream.file_info.push(i),
                Record::DstreamId { refcnt, .. } => stream.refcnt = Some(refcnt),
                _ => unreachable!("filtered to stream records"),
            }
        }
        if self.seal.is_some() {
            if !stream.extents.is_empty() {
                return Err(invalid!("stream {id}: file extents in a sealed fs tree"));
            }
            stream.extents = self.extents(id)?;
        }
        Ok(stream)
    }
}
