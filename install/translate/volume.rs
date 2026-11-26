// SPDX-License-Identifier: GPL-3.0-or-later
//! A role's volume, read straight from its decoded member: its regular files, and their contents as the kernel would
//! give them, decompressed and, on a sealed volume, checked against the seal's hashes.

use rayon::prelude::*;
use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::BlockSource;
use switch_apfs_read::{Container, Entry, Fs, Step, Stream, XattrData, stream};

use crate::Error;
use crate::buffer::Buffer;

/// A source that the worker threads can share.
pub type Source<'a> = &'a (dyn BlockSource + Sync);

const APFS_FILE_INFO_DATA_HASH: u8 = 1;

#[derive(Clone, Debug)]
pub enum Data {
    Empty,
    Plain(Stream),
    Compressed { decmpfs: Xattr, fork: Option<Stream> },
}

#[derive(Clone, Debug)]
pub enum Xattr {
    Embedded(Vec<u8>),
    Stream(Stream),
}

#[derive(Clone, Debug)]
pub struct File {
    /// `/`-separated from the volume's root, with a leading `/`.
    pub path: String,
    pub data: Data,
}

pub struct Volume<'a> {
    pub fs: Fs<'a>,
    pub sealed: bool,
    src: Source<'a>,
    container: Container,
    index: usize,
}

/// Directories this deep are walked side by side, each with its own reader of the volume.
const SPLIT_DEPTH: usize = 3;

impl<'a> Volume<'a> {
    /// The volume `uuid` names, or the container's only volume.
    pub fn open(src: Source<'a>, uuid: Option<[u8; 16]>) -> Result<Self, Error> {
        let container = Container::open(src)?;
        let index = match uuid {
            Some(uuid) => container
                .volumes
                .iter()
                .position(|v| v.sb.vol_uuid == uuid)
                .ok_or_else(|| Error::Refused(format!("no volume {}", hex::encode(uuid))))?,
            None if container.volumes.len() == 1 => 0,
            None => {
                let n = container.volumes.len();
                return Err(Error::Refused(format!("{n} volumes, expected one")));
            }
        };
        let fs = Fs::open(src, &container.volumes[index])?;
        let sealed = fs.seal.is_some();
        Ok(Self {
            fs,
            sealed,
            src,
            container,
            index,
        })
    }

    /// Every regular file, in no set order, except those under a directory whose path starts with one of `skip`.
    pub fn files(&self, skip: &[&str]) -> Result<Vec<File>, Error> {
        let mut files = Vec::new();
        let mut split = Vec::new();
        self.fs.walk("/", &mut |e: &Entry| {
            if e.inode.is_dir() && e.path.matches('/').count() >= SPLIT_DEPTH {
                split.push(e.path.clone());
                return Ok(Step::Over);
            }
            visit(e, skip, &mut files)
        })?;
        let (src, volume) = (self.src, &self.container.volumes[self.index]);
        let deeper = split
            .par_iter()
            .map(|dir| {
                let fs = Fs::open(src, volume)?;
                let mut found = Vec::new();
                fs.walk(dir, &mut |e: &Entry| visit(e, skip, &mut found))?;
                Ok(found)
            })
            .collect::<Result<Vec<_>, Error>>()?;
        files.extend(deeper.into_iter().flatten());
        Ok(files)
    }

    pub fn file(&self, path: &str) -> Result<Option<File>, Error> {
        let Some(oid) = self.fs.lookup(path)? else {
            return Ok(None);
        };
        let (entry, _) = self.fs.entry(oid, path.to_owned(), None)?;
        Ok(if entry.inode.is_regular() {
            Some(file(&entry)?)
        } else {
            None
        })
    }

    pub fn is_dir(&self, path: &str) -> Result<bool, Error> {
        let Some(oid) = self.fs.lookup(path)? else {
            return Ok(false);
        };
        Ok(self.fs.inode(oid)?.is_some_and(|i| i.is_dir()))
    }
}

fn visit(e: &Entry, skip: &[&str], files: &mut Vec<File>) -> Result<Step, switch_apfs_read::ReadError> {
    if e.inode.is_dir() {
        return Ok(Step::Into);
    }
    let parent = e.path.rsplit_once('/').map_or("", |(p, _)| p);
    if e.inode.is_regular() && !skip.iter().any(|s| parent.starts_with(s)) {
        files.push(file(e)?);
    }
    Ok(Step::Over)
}

fn file(e: &Entry) -> Result<File, switch_apfs_read::ReadError> {
    let data = if e.inode.is_compressed() {
        let decmpfs = match e.xattr(switch_decmpfs::XATTR) {
            Some((x, stream)) => match (&x.data, stream) {
                (XattrData::Embedded(b), _) => Xattr::Embedded(b.clone()),
                (XattrData::Stream { .. }, Some(s)) => Xattr::Stream(s.clone()),
                (XattrData::Stream { id, .. }, None) => {
                    return Err(switch_apfs_read::ReadError::invalid(format!(
                        "{}: decmpfs stream {id}",
                        e.path
                    )));
                }
            },
            None => {
                return Err(switch_apfs_read::ReadError::invalid(format!(
                    "{}: compressed, no decmpfs",
                    e.path
                )));
            }
        };
        let fork = e.xattr(switch_decmpfs::RESOURCE_FORK).and_then(|(_, s)| s.clone());
        Data::Compressed { decmpfs, fork }
    } else {
        match &e.data {
            Some(s) => Data::Plain(s.clone()),
            None => Data::Empty,
        }
    };
    Ok(File {
        path: e.path.clone(),
        data,
    })
}

impl File {
    /// The file's bytes if it starts with a Mach-O magic. An uncompressed file is read only that far otherwise.
    pub fn macho_contents(&self, src: Source<'_>, sealed: bool) -> Result<Option<Buffer>, Error> {
        if let Data::Plain(s) = &self.data
            && !crate::macho::is_macho(&head(src, s).map_err(|e| e.at(&self.path))?)
        {
            return Ok(None);
        }
        let bytes = self.contents(src, sealed)?;
        Ok(crate::macho::is_macho(&bytes).then_some(bytes))
    }

    pub fn compression(&self, src: Source<'_>) -> Result<Option<switch_decmpfs::Header>, Error> {
        let Data::Compressed { decmpfs, .. } = &self.data else {
            return Ok(None);
        };
        let bytes = match decmpfs {
            Xattr::Embedded(b) => b.clone(),
            Xattr::Stream(s) => read(src, s)?.to_vec(),
        };
        Ok(Some(switch_decmpfs::Header::parse(&bytes)?))
    }

    /// The file's bytes as the kernel reads them.
    pub fn contents(&self, src: Source<'_>, sealed: bool) -> Result<Buffer, Error> {
        let at = |e: Error| e.at(&self.path);
        match &self.data {
            Data::Empty => Ok(Buffer::zeroed(0)),
            Data::Plain(s) => {
                let bytes = read(src, s).map_err(at)?;
                if sealed {
                    check_seal(&bytes, s).map_err(at)?;
                }
                Ok(bytes)
            }
            Data::Compressed { decmpfs, fork } => {
                let header = match decmpfs {
                    Xattr::Embedded(b) => b.clone(),
                    Xattr::Stream(s) => read(src, s).map_err(at)?.to_vec(),
                };
                let fork_bytes = fork.as_ref().map(|s| read(src, s)).transpose().map_err(at)?;
                let plain = switch_decmpfs::decompress(&header, fork_bytes.as_deref()).map_err(|e| at(e.into()))?;
                if sealed && let Some(s) = fork {
                    check_seal(&plain, s).map_err(at)?;
                }
                Ok(Buffer::from_vec(plain))
            }
        }
    }
}

fn read(src: Source<'_>, s: &Stream) -> Result<Buffer, Error> {
    let size = usize::try_from(s.size).map_err(|_| Error::Refused(format!("stream {}: {} bytes", s.id, s.size)))?;
    let mut out = Buffer::zeroed(size);
    let mut at = 0;
    stream::read(src, s, &mut |chunk| {
        out[at..at + chunk.len()].copy_from_slice(chunk);
        at += chunk.len();
    })?;
    Ok(out)
}

fn head(src: Source<'_>, s: &Stream) -> Result<Vec<u8>, Error> {
    let Some(first) = s.extents.iter().find(|e| e.logical_addr == 0) else {
        return Ok(Vec::new());
    };
    let mut block = vec![0u8; BLOCK_SIZE];
    if first.phys_block_num != 0 {
        src.read_blocks(first.phys_block_num, &mut block)?;
    }
    block.truncate(s.size.min(first.len).min(BLOCK_SIZE as u64) as usize);
    Ok(block)
}

/// The seal hashes a stream's plaintext: SHA-256 over each stretch, the last zero-padded to whole blocks.
fn check_seal(plain: &[u8], s: &Stream) -> Result<(), Error> {
    for info in &s.file_info {
        let start = usize::try_from(info.offset).unwrap_or(usize::MAX);
        let end = start.saturating_add(usize::from(info.hashed_len) * BLOCK_SIZE);
        if info.info_type != APFS_FILE_INFO_DATA_HASH || start > plain.len() {
            return Err(Error::Refused(format!("a seal hash at byte {start}, past the end")));
        }
        let mut h = Sha256::new();
        let have = &plain[start..end.min(plain.len())];
        h.update(have);
        h.update(vec![0u8; end - start - have.len()]);
        if h.finalize()[..] != info.hash[..] {
            return Err(Error::Refused(format!("bytes {start}..{end} fail the volume's seal")));
        }
    }
    Ok(())
}
