// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Cursor};

use plist::{Dictionary, Value as PlistValue};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME, UF_COMPRESSED};
use switch_apfs_format::io::BlockTarget;
use switch_apfs_read::{FileExtent, Fs};
use switch_apfs_write::{Content, InodeRef, Kind, StreamRef, Tree, XattrValue};

use crate::Error;
use crate::manifest::PrefFile;

pub(crate) fn prepare_merged_plist(
    tree: &Tree,
    source_fs: Option<&Fs<'_>>,
    plist_path: &str,
    pref: &PrefFile,
) -> Result<Vec<u8>, Error> {
    let mut dict = match tree.lookup(plist_path) {
        Some(existing) => {
            let bytes = read_tree_file_bytes(tree, source_fs, existing, plist_path)?;
            parse_plist_dict(&bytes, plist_path)?
        }
        None => Dictionary::new(),
    };
    for (key, value) in &pref.values {
        dict.insert(key.clone(), value.clone());
    }
    let mut encoded = Vec::new();
    plist::to_writer_binary(&mut encoded, &PlistValue::Dictionary(dict)).map_err(|e| Error::InvalidPlist {
        path: plist_path.to_owned(),
        detail: e.to_string(),
    })?;
    Ok(encoded)
}

fn parse_plist_dict(bytes: &[u8], path: &str) -> Result<Dictionary, Error> {
    let value = PlistValue::from_reader(Cursor::new(bytes)).map_err(|e| Error::InvalidPlist {
        path: path.to_owned(),
        detail: e.to_string(),
    })?;
    match value {
        PlistValue::Dictionary(dict) => Ok(dict),
        _ => Err(Error::InvalidPlist {
            path: path.to_owned(),
            detail: "root value is not a dictionary".into(),
        }),
    }
}

fn read_tree_file_bytes(
    tree: &Tree,
    source_fs: Option<&Fs<'_>>,
    inode: InodeRef,
    path: &str,
) -> Result<Vec<u8>, Error> {
    let node = tree.inode(inode);
    if node.kind != Kind::File {
        return Err(Error::NotRegularFile(path.to_owned()));
    }
    if node.attrs.bsd_flags & UF_COMPRESSED != 0 {
        let decmpfs = read_tree_xattr_bytes(tree, source_fs, inode, DECMPFS_XATTR_NAME, path)?.ok_or_else(|| {
            Error::InvalidPlist {
                path: path.to_owned(),
                detail: "compressed file is missing com.apple.decmpfs".into(),
            }
        })?;
        let fork = read_tree_xattr_bytes(tree, source_fs, inode, RESOURCE_FORK_XATTR_NAME, path)?;
        return Ok(switch_decmpfs::decompress(&decmpfs, fork.as_deref())?);
    }
    match node.data {
        None => Ok(Vec::new()),
        Some(stream_ref) => read_tree_stream_bytes(tree, source_fs, stream_ref, path),
    }
}

fn read_tree_xattr_bytes(
    tree: &Tree,
    source_fs: Option<&Fs<'_>>,
    inode: InodeRef,
    name: &str,
    path: &str,
) -> Result<Option<Vec<u8>>, Error> {
    let Some(xattr) = tree.inode(inode).xattrs.iter().find(|x| x.name == name) else {
        return Ok(None);
    };
    match &xattr.value {
        XattrValue::Embedded(bytes) => Ok(Some(bytes.clone())),
        XattrValue::Stream(stream_ref) => Ok(Some(read_tree_stream_bytes(tree, source_fs, *stream_ref, path)?)),
    }
}

fn read_tree_stream_bytes(
    tree: &Tree,
    source_fs: Option<&Fs<'_>>,
    stream_ref: StreamRef,
    path: &str,
) -> Result<Vec<u8>, Error> {
    let stream = tree.stream(stream_ref);
    match &stream.content {
        Content::Bytes(bytes) => Ok(bytes.to_vec()),
        Content::Source(extents) => {
            let fs = source_fs.ok_or_else(|| Error::MissingSourceFs(path.to_owned()))?;
            let cap = usize::try_from(stream.size).map_err(|_| Error::InvalidPlist {
                path: path.to_owned(),
                detail: format!("stream size {} exceeds addressable memory", stream.size),
            })?;
            let read_stream = switch_apfs_read::Stream {
                id: 0,
                size: stream.size,
                extents: extents
                    .iter()
                    .map(|e| FileExtent {
                        stream: 0,
                        logical_addr: e.logical,
                        len: e.len,
                        flags: e.flags,
                        phys_block_num: e.paddr,
                        crypto_id: e.crypto_id,
                    })
                    .collect(),
                file_info: Vec::new(),
                refcnt: None,
            };
            let mut out = Vec::with_capacity(cap);
            switch_apfs_read::stream::read(fs.source(), &read_stream, &mut |chunk| {
                out.extend_from_slice(chunk);
            })?;
            Ok(out)
        }
        Content::Fill(fill) => {
            let cap = usize::try_from(stream.size).map_err(|_| Error::InvalidPlist {
                path: path.to_owned(),
                detail: format!("stream size {} exceeds addressable memory", stream.size),
            })?;
            let blocks = stream.size.div_ceil(BLOCK_SIZE as u64);
            let mut target = BufferTarget::new(blocks, path)?;
            fill.fill(&mut target)?;
            let mut out = target.bytes;
            out.truncate(cap);
            Ok(out)
        }
    }
}

struct BufferTarget {
    bytes: Vec<u8>,
}

impl BufferTarget {
    fn new(blocks: u64, path: &str) -> Result<Self, Error> {
        let len = usize::try_from(blocks)
            .ok()
            .and_then(|b| b.checked_mul(BLOCK_SIZE))
            .ok_or_else(|| Error::InvalidPlist {
                path: path.to_owned(),
                detail: format!("{blocks} blocks exceed addressable memory"),
            })?;
        Ok(Self { bytes: vec![0u8; len] })
    }
}

impl BlockTarget for BufferTarget {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        let start = usize::try_from(paddr)
            .ok()
            .and_then(|p| p.checked_mul(BLOCK_SIZE))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "block offset overflow"))?;
        let end = start
            .checked_add(data.len())
            .filter(|&e| e <= self.bytes.len() && data.len().is_multiple_of(BLOCK_SIZE))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "write past fill buffer"))?;
        self.bytes[start..end].copy_from_slice(data);
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
