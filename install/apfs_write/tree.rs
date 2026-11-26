// SPDX-License-Identifier: GPL-3.0-or-later
//! What a new volume holds: inodes with every attribute APFS stores, the names that link them into directories, and
//! the data streams their files and xattrs read from. Inode numbers are handed out as entries are added, so a caller
//! can record them (the pretranslated tree's `.index`) before any content exists.

use std::collections::HashMap;
use std::sync::Arc;

use switch_apfs_format::fs::{MIN_USER_INO_NUM, ROOT_DIR_INO_NUM};
use switch_apfs_format::fs_records::{
    DECMPFS_XATTR_NAME, INO_EXT_TYPE_SPARSE_BYTES, RESOURCE_FORK_XATTR_NAME, SYMLINK_XATTR_NAME, UF_COMPRESSED,
    XATTR_DATA_EMBEDDED, XATTR_FILE_SYSTEM_OWNED, XATTR_MAX_EMBEDDED_SIZE,
};
use switch_apfs_read::XField;
use unicode_normalization::UnicodeNormalization;

use crate::WriteError;
use crate::content::Content;

/// A bit the Reference doesn't name, which macOS's setxattr sets on every xattr it stores in a stream.
const XATTR_SETXATTR_STREAM: u16 = 0x10;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct InodeRef(pub(crate) u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct StreamRef(pub(crate) u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Dir,
    File,
    Symlink,
}

/// Nanoseconds since 1970, as APFS stores them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Times {
    pub create: u64,
    pub modify: u64,
    pub change: u64,
    pub access: u64,
}

impl Times {
    pub fn all(t: u64) -> Self {
        Self {
            create: t,
            modify: t,
            change: t,
            access: t,
        }
    }
}

/// An inode's own attributes. `times: None` takes the build time from `WriteOptions`.
#[derive(Clone, Debug)]
pub struct Attrs {
    pub uid: u32,
    pub gid: u32,
    /// Permission bits, setuid, setgid and sticky; the file type comes from the inode's kind.
    pub mode: u16,
    pub bsd_flags: u32,
    pub times: Option<Times>,
}

impl Attrs {
    pub fn new(uid: u32, gid: u32, mode: u16) -> Self {
        Self {
            uid,
            gid,
            mode,
            bsd_flags: 0,
            times: None,
        }
    }
}

#[derive(Clone, Debug)]
pub enum XattrValue {
    Embedded(Vec<u8>),
    Stream(StreamRef),
}

#[derive(Clone, Debug)]
pub struct Xattr {
    pub name: String,
    /// As stored; the writer sets the embedded and stream bits from the value.
    pub flags: u16,
    pub value: XattrValue,
}

/// A name of an inode in a directory.
#[derive(Clone, Debug)]
pub struct Link {
    pub parent: InodeRef,
    pub name: String,
    /// The hash a copied name was stored with; a new name is hashed by the writer.
    pub hash: Option<u32>,
    /// The directory record's `date_added`; the build time when none.
    pub date_added: Option<u64>,
    /// Directory-record xfields kept from a copy, the sibling id excepted.
    pub xfields: Vec<XField>,
}

/// Fields a verbatim copy keeps as the source stored them.
#[derive(Clone, Debug, Default)]
pub struct Stored {
    /// The source's, which copied files without a data stream (compressed ones) share when they are clones; None once
    /// `replace_content` gives the file content of its own.
    pub private_id: Option<u64>,
    pub internal_flags: u64,
    pub default_protection_class: u32,
    pub write_generation_counter: u32,
    /// A file's; a directory's is derived from the target volume's features.
    pub uncompressed_size: u64,
    /// Every inode xfield in the source's order. The name and the data stream's fields are rewritten in place.
    pub xfields: Vec<XField>,
}

#[derive(Clone, Debug)]
pub struct Inode {
    pub kind: Kind,
    pub attrs: Attrs,
    pub xattrs: Vec<Xattr>,
    pub data: Option<StreamRef>,
    /// The first is the primary link: the inode's own parent and name.
    pub links: Vec<Link>,
    pub stored: Option<Stored>,
    pub(crate) number: u64,
}

/// A data stream, shared by every inode or xattr that names it, as a whole-file clone shares one.
#[derive(Clone, Debug)]
pub struct Stream {
    pub size: u64,
    pub content: Content,
    /// A copied stream's `j_dstream`: allocated size, crypto id, bytes written and read.
    pub stored: Option<StoredDstream>,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct StoredDstream {
    pub alloced_size: u64,
    pub default_crypto_id: u64,
    pub total_bytes_written: u64,
    pub total_bytes_read: u64,
}

pub struct Tree {
    pub(crate) inodes: Vec<Inode>,
    pub(crate) streams: Vec<Stream>,
    /// Keyed by each name's NFD: a normalisation-insensitive volume finds one entry for every form of a name.
    children: HashMap<(InodeRef, String), InodeRef>,
    next_number: u64,
}

impl Tree {
    pub fn new(root: Attrs) -> Self {
        Self {
            inodes: vec![Inode {
                kind: Kind::Dir,
                attrs: root,
                xattrs: Vec::new(),
                data: None,
                links: Vec::new(),
                stored: None,
                number: ROOT_DIR_INO_NUM,
            }],
            streams: Vec::new(),
            children: HashMap::new(),
            next_number: MIN_USER_INO_NUM,
        }
    }

    pub fn root(&self) -> InodeRef {
        InodeRef(0)
    }

    /// The inode number the volume will store.
    pub fn number(&self, inode: InodeRef) -> u64 {
        self.inodes[inode.0 as usize].number
    }

    pub fn inode(&self, inode: InodeRef) -> &Inode {
        &self.inodes[inode.0 as usize]
    }

    pub fn inode_mut(&mut self, inode: InodeRef) -> &mut Inode {
        &mut self.inodes[inode.0 as usize]
    }

    pub fn stream(&self, stream: StreamRef) -> &Stream {
        &self.streams[stream.0 as usize]
    }

    /// Per stream, whether an inode or an xattr reads it.
    pub(crate) fn used_streams(&self) -> Vec<bool> {
        let mut used = vec![false; self.streams.len()];
        for inode in &self.inodes {
            let xattr_streams = inode.xattrs.iter().filter_map(|x| match x.value {
                XattrValue::Stream(s) => Some(s),
                XattrValue::Embedded(_) => None,
            });
            for s in inode.data.into_iter().chain(xattr_streams) {
                used[s.0 as usize] = true;
            }
        }
        used
    }

    pub fn inode_count(&self) -> usize {
        self.inodes.len()
    }

    pub fn inode_refs(&self) -> impl Iterator<Item = InodeRef> {
        (0..self.inodes.len() as u32).map(InodeRef)
    }

    /// The next inode number the tree would hand out; the writer numbers siblings and xattr streams from here.
    pub fn next_number(&self) -> u64 {
        self.next_number
    }

    /// The entry `name` names in `dir`, in any normalisation form.
    pub fn child(&self, dir: InodeRef, name: &str) -> Option<InodeRef> {
        self.children.get(&(dir, name.nfd().collect())).copied()
    }

    /// The inode at a `/`-separated path from the root.
    pub fn lookup(&self, path: &str) -> Option<InodeRef> {
        path.split('/')
            .filter(|p| !p.is_empty())
            .try_fold(self.root(), |dir, name| self.child(dir, name))
    }

    pub fn add_stream(&mut self, size: u64, content: Content) -> StreamRef {
        self.streams.push(Stream {
            size,
            content,
            stored: None,
        });
        StreamRef(self.streams.len() as u32 - 1)
    }

    pub fn add_stored_stream(&mut self, size: u64, content: Content, stored: StoredDstream) -> StreamRef {
        let s = self.add_stream(size, content);
        self.streams[s.0 as usize].stored = Some(stored);
        s
    }

    pub fn add_bytes(&mut self, bytes: impl Into<Arc<[u8]>>) -> StreamRef {
        let bytes: Arc<[u8]> = bytes.into();
        self.add_stream(bytes.len() as u64, Content::Bytes(bytes))
    }

    pub fn add_dir(&mut self, parent: InodeRef, name: &str, attrs: Attrs) -> Result<InodeRef, WriteError> {
        self.add(parent, name, Kind::Dir, attrs, None)
    }

    /// A regular file; `data: None` is an empty file without a data stream.
    pub fn add_file(
        &mut self,
        parent: InodeRef,
        name: &str,
        attrs: Attrs,
        data: Option<StreamRef>,
    ) -> Result<InodeRef, WriteError> {
        self.add(parent, name, Kind::File, attrs, data)
    }

    /// A symlink, its target stored as APFS stores one: a file-system-owned embedded xattr with the NUL.
    pub fn add_symlink(
        &mut self,
        parent: InodeRef,
        name: &str,
        attrs: Attrs,
        target: &str,
    ) -> Result<InodeRef, WriteError> {
        let inode = self.add(parent, name, Kind::Symlink, attrs, None)?;
        let mut value = target.as_bytes().to_vec();
        value.push(0);
        self.inodes[inode.0 as usize].xattrs.push(Xattr {
            name: SYMLINK_XATTR_NAME.into(),
            flags: XATTR_DATA_EMBEDDED | XATTR_FILE_SYSTEM_OWNED,
            value: XattrValue::Embedded(value),
        });
        Ok(inode)
    }

    /// Another name for an existing file: a hard link.
    pub fn add_link(&mut self, inode: InodeRef, parent: InodeRef, name: &str) -> Result<(), WriteError> {
        if self.inode(inode).kind == Kind::Dir {
            return Err(WriteError::Spec(format!(
                "{name}: a directory can't have a second name"
            )));
        }
        self.link(inode, parent, name, None, None, Vec::new())
    }

    /// Sets an xattr, embedded when it fits and in a stream otherwise; replaces one of the same name.
    pub fn set_xattr(&mut self, inode: InodeRef, name: &str, value: &[u8]) {
        let (flags, value) = if value.len() <= XATTR_MAX_EMBEDDED_SIZE {
            (0, XattrValue::Embedded(value.to_vec()))
        } else {
            (
                XATTR_SETXATTR_STREAM,
                XattrValue::Stream(self.add_bytes(value.to_vec())),
            )
        };
        self.put_xattr(
            inode,
            Xattr {
                name: name.into(),
                flags,
                value,
            },
        );
    }

    pub fn put_xattr(&mut self, inode: InodeRef, xattr: Xattr) {
        let xattrs = &mut self.inodes[inode.0 as usize].xattrs;
        match xattrs.iter_mut().find(|x| x.name == xattr.name) {
            Some(x) => *x = xattr,
            None => xattrs.push(xattr),
        }
    }

    /// Gives a file new content, stored uncompressed: a compressed file loses its decmpfs and resource fork and its
    /// `UF_COMPRESSED`, and keeps its inode, owner, mode and other flags. A clone stops being one. The streams it
    /// read before are left out of the volume when nothing else reads them.
    pub fn replace_content(&mut self, inode: InodeRef, data: Option<StreamRef>) -> Result<(), WriteError> {
        let node = &mut self.inodes[inode.0 as usize];
        if node.kind != Kind::File {
            return Err(WriteError::Spec("only a regular file has content to replace".into()));
        }
        node.xattrs
            .retain(|x| x.name != DECMPFS_XATTR_NAME && x.name != RESOURCE_FORK_XATTR_NAME);
        node.attrs.bsd_flags &= !UF_COMPRESSED;
        node.data = data;
        if let Some(stored) = &mut node.stored {
            stored.private_id = None;
            stored.internal_flags = crate::inode::replaced_flags(stored.internal_flags);
            stored.uncompressed_size = 0;
            stored.xfields.retain(|x| x.kind != INO_EXT_TYPE_SPARSE_BYTES);
        }
        Ok(())
    }

    pub(crate) fn add(
        &mut self,
        parent: InodeRef,
        name: &str,
        kind: Kind,
        attrs: Attrs,
        data: Option<StreamRef>,
    ) -> Result<InodeRef, WriteError> {
        if kind != Kind::File && data.is_some() {
            return Err(WriteError::Spec(format!(
                "{name}: only a regular file has a data stream"
            )));
        }
        self.vacant(parent, name)?;
        let inode = self.push(kind, attrs, data, None);
        self.link(inode, parent, name, None, None, Vec::new())?;
        Ok(inode)
    }

    /// The key `name` would take in `parent`, which must be a directory without the name in any normalisation form.
    fn vacant(&self, parent: InodeRef, name: &str) -> Result<(InodeRef, String), WriteError> {
        if self.inode(parent).kind != Kind::Dir {
            return Err(WriteError::Spec(format!("{name}: its parent isn't a directory")));
        }
        let key = (parent, name.nfd().collect());
        if self.children.contains_key(&key) {
            return Err(WriteError::Spec(format!(
                "{name}: its directory already holds the name, in this or another normalisation form"
            )));
        }
        Ok(key)
    }

    /// A new inode with no name yet; `link` gives it one.
    pub(crate) fn push(
        &mut self,
        kind: Kind,
        attrs: Attrs,
        data: Option<StreamRef>,
        stored: Option<Stored>,
    ) -> InodeRef {
        let inode = InodeRef(self.inodes.len() as u32);
        self.inodes.push(Inode {
            kind,
            attrs,
            xattrs: Vec::new(),
            data,
            links: Vec::new(),
            stored,
            number: self.next_number,
        });
        self.next_number += 1;
        inode
    }

    pub(crate) fn link(
        &mut self,
        inode: InodeRef,
        parent: InodeRef,
        name: &str,
        hash: Option<u32>,
        date_added: Option<u64>,
        xfields: Vec<XField>,
    ) -> Result<(), WriteError> {
        let key = self.vacant(parent, name)?;
        self.children.insert(key, inode);
        self.inodes[inode.0 as usize].links.push(Link {
            parent,
            name: name.into(),
            hash,
            date_added,
            xfields,
        });
        Ok(())
    }
}
