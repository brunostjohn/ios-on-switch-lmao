// SPDX-License-Identifier: GPL-3.0-or-later
//! decmpfs, APFS's transparent compression: a compressed file's contents from its `com.apple.decmpfs` xattr and, for
//! the even types, its `com.apple.ResourceFork`. LZBITMAP (13, 14) goes to `zbitmap`, every other type to `cmpfs`.

use std::io::Cursor;

pub const XATTR: &str = "com.apple.decmpfs";
pub const RESOURCE_FORK: &str = "com.apple.ResourceFork";

const MAGIC: &[u8; 4] = b"fpmc";
const HEADER_LEN: usize = 16;
const LZBITMAP_XATTR: u32 = 13;
const LZBITMAP_FORK: u32 = 14;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("decmpfs header: {0}")]
    Header(&'static str),
    #[error("decmpfs type {0} keeps its data in a resource fork, and there is none")]
    NoResourceFork(u32),
    #[error("decmpfs type {kind}: {size} bytes don't fit in memory")]
    TooLarge { kind: u32, size: u64 },
    #[error("decmpfs: {0}")]
    Cmpfs(#[from] cmpfs::CmpfsError),
    #[error("decmpfs LZBITMAP: {0}")]
    Lzbitmap(#[from] zbitmap::decmpfs::Error),
    #[error("decmpfs type {kind}: {got} bytes, the header says {size}")]
    Size { kind: u32, got: u64, size: u64 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub kind: u32,
    /// The file's size once decompressed.
    pub size: u64,
}

impl Header {
    pub fn parse(xattr: &[u8]) -> Result<Self, Error> {
        if xattr.len() < HEADER_LEN {
            return Err(Error::Header("shorter than 16 bytes"));
        }
        if &xattr[..4] != MAGIC {
            return Err(Error::Header("magic is not fpmc"));
        }
        let kind = u32::from_le_bytes(xattr[4..8].try_into().expect("four bytes"));
        let size = u64::from_le_bytes(xattr[8..16].try_into().expect("eight bytes"));
        Ok(Self { kind, size })
    }

    /// Whether the data is in the resource fork rather than after the header.
    pub fn in_resource_fork(&self) -> bool {
        matches!(self.kind, 4 | 8 | 10 | 12 | 14)
    }
}

/// The file's contents. Refuses any type it doesn't know and any result whose size isn't the header's.
pub fn decompress(xattr: &[u8], resource_fork: Option<&[u8]>) -> Result<Vec<u8>, Error> {
    let header = Header::parse(xattr)?;
    if header.in_resource_fork() && resource_fork.is_none() {
        return Err(Error::NoResourceFork(header.kind));
    }
    let out = match header.kind {
        LZBITMAP_XATTR | LZBITMAP_FORK => lzbitmap(header, xattr, resource_fork)?,
        _ => cmpfs::decompress(xattr, resource_fork)?,
    };
    if out.len() as u64 != header.size {
        return Err(Error::Size {
            kind: header.kind,
            got: out.len() as u64,
            size: header.size,
        });
    }
    Ok(out)
}

fn lzbitmap(header: Header, xattr: &[u8], resource_fork: Option<&[u8]>) -> Result<Vec<u8>, Error> {
    let capacity = usize::try_from(header.size).map_err(|_| Error::TooLarge {
        kind: header.kind,
        size: header.size,
    })?;
    let mut out = Vec::with_capacity(capacity);
    if header.kind == LZBITMAP_XATTR {
        zbitmap::decmpfs::decode_attr(xattr, &mut out)?;
    } else {
        let parsed = zbitmap::decmpfs::parse_header(xattr)?;
        let fork = Cursor::new(resource_fork.expect("checked by decompress"));
        zbitmap::decmpfs::decode_rsrc(parsed, fork, &mut out)?;
    }
    Ok(out)
}

#[cfg(test)]
mod tests;
