// SPDX-License-Identifier: GPL-3.0-or-later
//! Serializes a planned `switch_apfs_write::Tree` into the canonical JSON tree dump format used by `tree_diff.py`.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io;
use std::path::Path;

use serde::Serialize;
use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME, SYMLINK_XATTR_NAME, UF_COMPRESSED};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_write::{Content, Fill, InodeRef, Kind, SourceExtent, StreamRef, Tree, XattrValue};

use crate::{Error, Result};

#[derive(Debug, Serialize)]
pub struct TreeDump {
    pub version: u32,
    pub source: &'static str,
    pub entries: BTreeMap<String, EntryDump>,
    pub hardlink_groups: Vec<Vec<String>>,
}

#[derive(Debug, Serialize)]
pub struct EntryDump {
    #[serde(rename = "type")]
    pub kind: &'static str,
    pub uid: u32,
    pub gid: u32,
    pub mode: String,
    pub flags: u32,
    pub size: u64,
    pub sha256: Option<String>,
    pub xattrs: BTreeMap<String, XattrDump>,
    pub acl: Option<Vec<String>>,
    pub symlink_target: Option<String>,
    pub mtime_ns: u64,
    pub ino: u64,
    pub nlink: u64,
}

#[derive(Debug, Serialize)]
pub struct XattrDump {
    pub size: u64,
    pub sha256: String,
}

pub fn dump_tree(tree: &Tree, src: &dyn BlockSource, time_ns: u64, expanded_records: bool) -> Result<TreeDump> {
    let mut children: HashMap<InodeRef, Vec<(String, InodeRef)>> = HashMap::new();
    for iref in tree.inode_refs() {
        for link in &tree.inode(iref).links {
            children.entry(link.parent).or_default().push((link.name.clone(), iref));
        }
    }
    for list in children.values_mut() {
        list.sort_by(|a, b| a.0.cmp(&b.0));
    }

    let mut entries = BTreeMap::new();
    let mut by_ino: BTreeMap<u64, Vec<String>> = BTreeMap::new();
    let mut stack = vec![(tree.root(), "/".to_owned())];
    while let Some((iref, path)) = stack.pop() {
        let entry = build_entry(tree, iref, src, time_ns, expanded_records, &children)?;
        if entry.kind != "d" {
            by_ino.entry(entry.ino).or_default().push(path.clone());
        }
        entries.insert(path.clone(), entry);
        if tree.inode(iref).kind == Kind::Dir
            && let Some(kids) = children.get(&iref)
        {
            let base = if path == "/" { "" } else { path.as_str() };
            for (name, child) in kids.iter().rev() {
                stack.push((*child, format!("{base}/{name}")));
            }
        }
    }

    let mut hardlink_groups: Vec<Vec<String>> = by_ino
        .into_values()
        .filter(|paths| paths.len() > 1)
        .map(|mut paths| {
            paths.sort();
            paths
        })
        .collect();
    hardlink_groups.sort();

    Ok(TreeDump {
        version: 1,
        source: "plan",
        entries,
        hardlink_groups,
    })
}

pub fn write_json(path: &Path, dump: &TreeDump) -> Result<()> {
    let bytes = serde_json::to_vec_pretty(dump)?;
    fs::write(path, bytes)?;
    Ok(())
}

fn build_entry(
    tree: &Tree,
    iref: InodeRef,
    src: &dyn BlockSource,
    time_ns: u64,
    expanded_records: bool,
    children: &HashMap<InodeRef, Vec<(String, InodeRef)>>,
) -> Result<EntryDump> {
    let node = tree.inode(iref);
    let ino = tree.number(iref);
    let mtime_ns = node.attrs.times.map_or(time_ns, |t| t.modify);
    let mode = format!("{:04o}", node.attrs.mode & 0o7777);

    let mut xattrs = BTreeMap::new();
    let mut symlink_target = None;
    let mut decmpfs_bytes = None;
    let mut rsrc_bytes = None;

    for x in &node.xattrs {
        let raw = read_xattr_bytes(tree, src, &x.value)?;
        if node.kind == Kind::Symlink && x.name == SYMLINK_XATTR_NAME {
            let trimmed = raw.strip_suffix(&[0]).unwrap_or(&raw);
            symlink_target = Some(String::from_utf8_lossy(trimmed).into_owned());
            continue;
        }
        if x.name == RESOURCE_FORK_XATTR_NAME && raw.is_empty() {
            continue;
        }
        if x.name == DECMPFS_XATTR_NAME {
            decmpfs_bytes = Some(raw.clone());
        } else if x.name == RESOURCE_FORK_XATTR_NAME {
            rsrc_bytes = Some(raw.clone());
        }
        xattrs.insert(
            x.name.clone(),
            XattrDump {
                size: raw.len() as u64,
                sha256: hex::encode(Sha256::digest(&raw)),
            },
        );
    }

    let (kind, size, sha256, nlink) = match node.kind {
        Kind::Dir => {
            let kids = children.get(&iref).map_or(0, Vec::len) as u64;
            let subdirs = children.get(&iref).map_or(0, |list| {
                list.iter().filter(|(_, c)| tree.inode(*c).kind == Kind::Dir).count() as u64
            });
            let links = if expanded_records { subdirs + 2 } else { kids + 2 };
            ("d", 32 * (kids + 2), None, links)
        }
        Kind::Symlink => {
            let target = symlink_target
                .as_deref()
                .ok_or_else(|| Error::Check(format!("symlink inode {ino} missing target")))?;
            ("l", target.len() as u64, None, node.links.len() as u64)
        }
        Kind::File => {
            let compressed = node.attrs.bsd_flags & UF_COMPRESSED != 0;
            let (sz, digest) = if compressed {
                let dec = decmpfs_bytes
                    .as_deref()
                    .ok_or_else(|| Error::Check(format!("compressed inode {ino} missing decmpfs")))?;
                let plain = switch_decmpfs::decompress(dec, rsrc_bytes.as_deref())?;
                (plain.len() as u64, hex::encode(Sha256::digest(&plain)))
            } else if let Some(sref) = node.data {
                hash_stream(tree, sref, src)?
            } else {
                (0, hex::encode(Sha256::digest([])))
            };
            ("f", sz, Some(digest), node.links.len() as u64)
        }
    };

    Ok(EntryDump {
        kind,
        uid: node.attrs.uid,
        gid: node.attrs.gid,
        mode,
        flags: node.attrs.bsd_flags,
        size,
        sha256,
        xattrs,
        acl: None,
        symlink_target,
        mtime_ns,
        ino,
        nlink,
    })
}

fn read_xattr_bytes(tree: &Tree, src: &dyn BlockSource, val: &XattrValue) -> Result<Vec<u8>> {
    match val {
        XattrValue::Embedded(b) => Ok(b.clone()),
        XattrValue::Stream(sref) => {
            let stream = tree.stream(*sref);
            let cap = usize::try_from(stream.size)
                .map_err(|_| Error::Check(format!("xattr stream {} too large", stream.size)))?;
            let mut out = Vec::with_capacity(cap);
            read_content(&stream.content, stream.size, src, &mut |chunk| {
                out.extend_from_slice(chunk)
            })?;
            Ok(out)
        }
    }
}

fn hash_stream(tree: &Tree, sref: StreamRef, src: &dyn BlockSource) -> Result<(u64, String)> {
    let stream = tree.stream(sref);
    let mut hasher = Sha256::new();
    read_content(&stream.content, stream.size, src, &mut |chunk| hasher.update(chunk))?;
    Ok((stream.size, hex::encode(hasher.finalize())))
}

fn read_content(content: &Content, size: u64, src: &dyn BlockSource, out: &mut dyn FnMut(&[u8])) -> Result<()> {
    match content {
        Content::Bytes(bytes) => {
            let n = (size as usize).min(bytes.len());
            out(&bytes[..n]);
            Ok(())
        }
        Content::Source(extents) => read_source_extents(src, extents, size, out),
        Content::Fill(fill) => read_fill_sequential(fill.as_ref(), size, out),
    }
}

fn read_source_extents(
    src: &dyn BlockSource,
    extents: &[SourceExtent],
    size: u64,
    out: &mut dyn FnMut(&[u8]),
) -> Result<()> {
    const CHUNK_BLOCKS: u64 = 256;
    const ZERO: [u8; BLOCK_SIZE] = [0; BLOCK_SIZE];
    let mut sorted = extents.to_vec();
    sorted.sort_by_key(|e| e.logical);
    let mut at = 0u64;
    let mut buf = vec![0u8; CHUNK_BLOCKS as usize * BLOCK_SIZE];
    let emit_zeros = |mut n: u64, sink: &mut dyn FnMut(&[u8])| {
        while n > 0 {
            let k = n.min(BLOCK_SIZE as u64) as usize;
            sink(&ZERO[..k]);
            n -= k as u64;
        }
    };
    for e in &sorted {
        emit_zeros(e.logical.min(size).saturating_sub(at), out);
        at = e.logical;
        let end = (e.logical + e.len).min(size);
        let mut blk = e.paddr;
        while at < end {
            let n = (end - at).div_ceil(BLOCK_SIZE as u64).min(CHUNK_BLOCKS);
            let bytes = (n * BLOCK_SIZE as u64).min(end - at) as usize;
            if e.paddr == 0 {
                emit_zeros(bytes as u64, out);
            } else {
                let chunk = &mut buf[..n as usize * BLOCK_SIZE];
                src.read_blocks(blk, chunk)?;
                out(&chunk[..bytes]);
                blk += n;
            }
            at += bytes as u64;
        }
        at = at.max(e.logical + e.len);
    }
    emit_zeros(size.saturating_sub(at), out);
    Ok(())
}

struct SequentialSink<'a> {
    expected_block: u64,
    remaining: u64,
    out: &'a mut dyn FnMut(&[u8]),
}

impl BlockTarget for SequentialSink<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        if !data.len().is_multiple_of(BLOCK_SIZE) || paddr != self.expected_block {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "non-sequential Fill write at block {paddr}, expected {}",
                    self.expected_block
                ),
            ));
        }
        let take = self.remaining.min(data.len() as u64) as usize;
        if take > 0 {
            (self.out)(&data[..take]);
            self.remaining -= take as u64;
        }
        self.expected_block += (data.len() / BLOCK_SIZE) as u64;
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn read_fill_sequential(fill: &dyn Fill, size: u64, out: &mut dyn FnMut(&[u8])) -> Result<()> {
    let mut sink = SequentialSink {
        expected_block: 0,
        remaining: size,
        out,
    };
    fill.fill(&mut sink)?;
    if sink.remaining != 0 {
        return Err(Error::Check(format!(
            "Fill ended with {} of {size} bytes unwritten",
            sink.remaining
        )));
    }
    Ok(())
}
