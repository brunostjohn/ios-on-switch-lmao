// SPDX-License-Identifier: GPL-3.0-or-later
//! An inode's value: `j_inode_val_t` and its xfields. A copied inode keeps what the source stored; a new one gets the
//! values macOS 27.2's kext gives the same kind of file.

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::{
    Dstream, INO_EXT_TYPE_DSTREAM, INO_EXT_TYPE_NAME, INODE_NO_RSRC_FORK, InodeVal, S_IFDIR, S_IFREG, XF_DO_NOT_COPY,
    XF_SYSTEM_FIELD, XfBlob,
};
use switch_apfs_format::fs_records::{RESOURCE_FORK_XATTR_NAME, S_IFLNK, UF_COMPRESSED};
use switch_apfs_read::XField;
use zerocopy::IntoBytes;
use zerocopy::little_endian::{I32, U16, U32, U64};

use crate::place::PlacedExtent;
use crate::tree::{Inode, Kind, Stream, Times};

pub const INODE_WAS_CLONED: u64 = 0x0000_0010;
pub const INODE_HAS_SECURITY_EA: u64 = 0x0000_0040;
pub const INODE_IS_SPARSE: u64 = 0x0000_0200;
pub const INODE_WAS_EVER_CLONED: u64 = 0x0000_0400;
pub const INODE_HAS_RSRC_FORK: u64 = 0x0000_4000;
pub const INODE_HAS_UNCOMPRESSED_SIZE: u64 = 0x0004_0000;
pub const SECURITY_XATTR_NAME: &str = "com.apple.system.Security";

/// What a file's flags become once it has new content of its own, stored uncompressed and dense: as macOS leaves a
/// rewritten clone, it was ever cloned but no longer is.
pub fn replaced_flags(flags: u64) -> u64 {
    (flags & !(INODE_HAS_UNCOMPRESSED_SIZE | INODE_HAS_RSRC_FORK | INODE_IS_SPARSE | INODE_WAS_CLONED))
        | INODE_NO_RSRC_FORK
}

fn derived_flags(inode: &Inode) -> u64 {
    let has = |name: &str| inode.xattrs.iter().any(|x| x.name == name);
    let mut flags = if has(RESOURCE_FORK_XATTR_NAME) {
        INODE_HAS_RSRC_FORK
    } else {
        INODE_NO_RSRC_FORK
    };
    if has(SECURITY_XATTR_NAME) {
        flags |= INODE_HAS_SECURITY_EA;
    }
    if inode.attrs.bsd_flags & UF_COMPRESSED != 0 {
        flags |= INODE_HAS_UNCOMPRESSED_SIZE;
    }
    flags
}

fn internal_flags(f: &InodeFields) -> u64 {
    let flags = f
        .inode
        .stored
        .as_ref()
        .map_or_else(|| derived_flags(f.inode), |s| s.internal_flags);
    if f.cloned {
        flags | INODE_WAS_CLONED | INODE_WAS_EVER_CLONED
    } else {
        flags
    }
}

pub fn mode_bits(kind: Kind) -> u16 {
    match kind {
        Kind::Dir => S_IFDIR,
        Kind::File => S_IFREG,
        Kind::Symlink => S_IFLNK,
    }
}

pub struct InodeFields<'a> {
    pub inode: &'a Inode,
    pub parent: u64,
    pub private_id: u64,
    /// One of a whole-file clone's inodes, which macOS marks as cloned on every side, whatever a source stored.
    pub cloned: bool,
    pub name: &'a str,
    /// A directory's children, or a file's links.
    pub nchildren_or_nlink: u32,
    pub uncompressed_size: u64,
    pub data: Option<(&'a Stream, &'a [PlacedExtent])>,
    pub time_ns: u64,
}

pub fn value(f: &InodeFields) -> Vec<u8> {
    let inode = f.inode;
    let t = inode.attrs.times.unwrap_or(Times::all(f.time_ns));
    let stored = inode.stored.as_ref();
    let val = InodeVal {
        parent_id: U64::new(f.parent),
        private_id: U64::new(f.private_id),
        create_time: U64::new(t.create),
        mod_time: U64::new(t.modify),
        change_time: U64::new(t.change),
        access_time: U64::new(t.access),
        internal_flags: U64::new(internal_flags(f)),
        nchildren_or_nlink: I32::new(f.nchildren_or_nlink as i32),
        default_protection_class: U32::new(stored.map_or(0, |s| s.default_protection_class)),
        write_generation_counter: U32::new(stored.map_or(0, |s| s.write_generation_counter)),
        bsd_flags: U32::new(inode.attrs.bsd_flags),
        owner: U32::new(inode.attrs.uid),
        group: U32::new(inode.attrs.gid),
        mode: U16::new(mode_bits(inode.kind) | (inode.attrs.mode & 0o7777)),
        pad1: U16::ZERO,
        uncompressed_size: U64::new(f.uncompressed_size),
    };
    let mut name = f.name.as_bytes().to_vec();
    name.push(0);
    let name = XField {
        kind: INO_EXT_TYPE_NAME,
        flags: XF_DO_NOT_COPY,
        data: name,
    };
    let dstream = f.data.map(|(s, e)| XField {
        kind: INO_EXT_TYPE_DSTREAM,
        flags: XF_SYSTEM_FIELD,
        data: dstream(s, e, true).as_bytes().to_vec(),
    });
    let fields = match stored {
        Some(s) => {
            let mut fields = Vec::with_capacity(s.xfields.len() + 2);
            let (mut name, mut dstream) = (Some(name), dstream);
            for x in &s.xfields {
                match x.kind {
                    INO_EXT_TYPE_NAME => fields.extend(name.take()),
                    INO_EXT_TYPE_DSTREAM => fields.extend(dstream.take()),
                    _ => fields.push(x.clone()),
                }
            }
            fields.extend(name);
            fields.extend(dstream);
            fields
        }
        None => std::iter::once(name).chain(dstream).collect(),
    };
    let mut out = val.as_bytes().to_vec();
    out.extend_from_slice(&xfields(&fields));
    out
}

/// A stream's `j_dstream`. A copied one keeps the source's counters; a new data stream counts its bytes as written,
/// a new xattr stream doesn't, as macOS's setxattr leaves it.
pub fn dstream(s: &Stream, extents: &[PlacedExtent], data: bool) -> Dstream {
    let alloced: u64 = extents
        .iter()
        .map(|e| e.len.div_ceil(BLOCK_SIZE as u64) * BLOCK_SIZE as u64)
        .sum();
    match s.stored {
        Some(d) => Dstream {
            size: U64::new(s.size),
            alloced_size: U64::new(d.alloced_size),
            default_crypto_id: U64::new(d.default_crypto_id),
            total_bytes_written: U64::new(d.total_bytes_written),
            total_bytes_read: U64::new(d.total_bytes_read),
        },
        None => Dstream {
            size: U64::new(s.size),
            alloced_size: U64::new(alloced),
            default_crypto_id: U64::ZERO,
            total_bytes_written: U64::new(if data { s.size } else { 0 }),
            total_bytes_read: U64::ZERO,
        },
    }
}

/// `xf_blob_t`: the field headers, then each field's data padded to 8 bytes.
pub fn xfields(fields: &[XField]) -> Vec<u8> {
    if fields.is_empty() {
        return Vec::new();
    }
    let data: usize = fields.iter().map(|x| x.data.len().next_multiple_of(8)).sum();
    let blob = XfBlob {
        num_exts: U16::new(fields.len() as u16),
        used_data: U16::new(data as u16),
    };
    let mut out = blob.as_bytes().to_vec();
    for x in fields {
        out.push(x.kind);
        out.push(x.flags);
        out.extend_from_slice(&(x.data.len() as u16).to_le_bytes());
    }
    for x in fields {
        out.extend_from_slice(&x.data);
        out.extend(std::iter::repeat_n(0, x.data.len().next_multiple_of(8) - x.data.len()));
    }
    out
}
