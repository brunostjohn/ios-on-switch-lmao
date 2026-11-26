// SPDX-License-Identifier: GPL-3.0-or-later
//! A data stream's bytes as stored, and the decmpfs header of a compressed file. Nothing here decompresses.

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{DECMPFS_HEADER_SIZE, DECMPFS_MAGIC, DECMPFS_XATTR_NAME, DecmpfsHeader};
use switch_apfs_format::get;
use switch_apfs_format::io::BlockSource;

use crate::catalog::XattrData;
use crate::subtree::{Entry, Stream};
use crate::{ReadError, invalid};

const CHUNK_BLOCKS: u64 = 256;

/// Feeds the stream's first `size` bytes to `out` in order, holes as zeros.
pub fn read(src: &dyn BlockSource, stream: &Stream, out: &mut dyn FnMut(&[u8])) -> Result<(), ReadError> {
    let mut extents = stream.extents.clone();
    extents.sort_by_key(|e| e.logical_addr);
    let mut at = 0u64;
    let mut buf = vec![0u8; CHUNK_BLOCKS as usize * BLOCK_SIZE];
    for e in &extents {
        if e.logical_addr < at {
            return Err(invalid!(
                "stream {}: extents overlap at byte {}",
                stream.id,
                e.logical_addr
            ));
        }
        zeros(e.logical_addr.min(stream.size).saturating_sub(at), out);
        at = e.logical_addr;
        let end = (e.logical_addr + e.len).min(stream.size);
        let mut block = e.phys_block_num;
        while at < end {
            let n = (end - at).div_ceil(BLOCK_SIZE as u64).min(CHUNK_BLOCKS);
            let bytes = (n * BLOCK_SIZE as u64).min(end - at) as usize;
            if e.phys_block_num == 0 {
                zeros(bytes as u64, out);
            } else {
                let chunk = &mut buf[..n as usize * BLOCK_SIZE];
                src.read_blocks(block, chunk)?;
                out(&chunk[..bytes]);
                block += n;
            }
            at += bytes as u64;
        }
        at = at.max(e.logical_addr + e.len);
    }
    zeros(stream.size.saturating_sub(at), out);
    Ok(())
}

fn zeros(mut n: u64, out: &mut dyn FnMut(&[u8])) {
    const ZERO: [u8; BLOCK_SIZE] = [0; BLOCK_SIZE];
    while n > 0 {
        let k = n.min(BLOCK_SIZE as u64) as usize;
        out(&ZERO[..k]);
        n -= k as u64;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Compression {
    pub kind: u32,
    /// What `stat` reports as the file's size.
    pub size: u64,
}

/// The `com.apple.decmpfs` header of a compressed file, embedded or in a stream.
pub fn compression(src: &dyn BlockSource, entry: &Entry) -> Result<Option<Compression>, ReadError> {
    let Some((xattr, stream)) = entry.xattr(DECMPFS_XATTR_NAME) else {
        return Ok(None);
    };
    let bytes = match (&xattr.data, stream) {
        (XattrData::Embedded(b), _) => b.clone(),
        (XattrData::Stream { .. }, Some(s)) => {
            let mut b = Vec::new();
            read(src, s, &mut |chunk| {
                if b.len() < DECMPFS_HEADER_SIZE {
                    b.extend_from_slice(chunk);
                }
            })?;
            b
        }
        (XattrData::Stream { id, .. }, None) => return Err(invalid!("decmpfs stream {id} without its extents")),
    };
    let h: DecmpfsHeader = get(&bytes, 0).ok_or_else(|| invalid!("{}: short decmpfs header", entry.path))?;
    if h.magic.get() != DECMPFS_MAGIC {
        return Err(invalid!("{}: decmpfs magic {:#x}", entry.path, h.magic.get()));
    }
    Ok(Some(Compression {
        kind: h.compression_type.get(),
        size: h.uncompressed_size.get(),
    }))
}
