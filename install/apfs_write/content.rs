// SPDX-License-Identifier: GPL-3.0-or-later
//! Where a data stream's bytes come from. Nothing here is read until phase C, and a rerun from phase B reads it
//! again, so every source must give the same bytes each time.

use std::fmt;
use std::io;
use std::sync::Arc;

use switch_apfs_alloc::Bitmap;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::BlockTarget;

#[derive(Clone)]
pub enum Content {
    Bytes(Arc<[u8]>),
    /// The source container's blocks, copied verbatim; holes stay holes.
    Source(Arc<[SourceExtent]>),
    /// Written in phase C into the stream's one contiguous extent.
    Fill(Arc<dyn Fill>),
}

impl fmt::Debug for Content {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Content::Bytes(b) => write!(f, "Bytes({})", b.len()),
            Content::Source(e) => write!(f, "Source({} extents)", e.len()),
            Content::Fill(_) => write!(f, "Fill"),
        }
    }
}

/// One `FILE_EXTENT` of a source stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceExtent {
    pub logical: u64,
    pub len: u64,
    pub flags: u8,
    /// The source block, or 0 for a hole.
    pub paddr: u64,
    pub crypto_id: u64,
}

impl SourceExtent {
    pub fn blocks(&self) -> u64 {
        self.len.div_ceil(BLOCK_SIZE as u64)
    }
}

/// Large new content, such as a cryptex image, written straight to its blocks.
pub trait Fill: Send + Sync {
    /// Writes the stream into `window`, whose block 0 is the stream's first block. The window ends at the stream's
    /// last block, and every block of it must be written; the tail past the stream's size must be zeros.
    fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()>;
}

/// A `Fill` that produces exactly `size` bytes in order.
pub struct Sequential<F> {
    size: u64,
    produce: F,
}

impl<F> Sequential<F> {
    pub fn new(size: u64, produce: F) -> Self {
        Self { size, produce }
    }
}

const FILL_BUFFER: usize = 8 << 20;

impl<F> Fill for Sequential<F>
where
    F: Fn(&mut dyn FnMut(&[u8]) -> io::Result<()>) -> io::Result<()> + Send + Sync,
{
    fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
        let mut buf = Vec::with_capacity(FILL_BUFFER);
        let mut at = 0u64;
        let mut produced = 0u64;
        (self.produce)(&mut |mut bytes: &[u8]| {
            produced += bytes.len() as u64;
            if produced > self.size {
                return Err(wrong_size(produced, self.size));
            }
            while !bytes.is_empty() {
                let take = bytes.len().min(FILL_BUFFER - buf.len());
                buf.extend_from_slice(&bytes[..take]);
                bytes = &bytes[take..];
                if buf.len() == FILL_BUFFER {
                    window.write_blocks(at, &buf)?;
                    at += (FILL_BUFFER / BLOCK_SIZE) as u64;
                    buf.clear();
                }
            }
            Ok(())
        })?;
        if produced != self.size {
            return Err(wrong_size(produced, self.size));
        }
        if !buf.is_empty() {
            buf.resize(buf.len().next_multiple_of(BLOCK_SIZE), 0);
            window.write_blocks(at, &buf)?;
        }
        Ok(())
    }
}

fn wrong_size(produced: u64, size: u64) -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        format!("a fill produced {produced} bytes for a {size}-byte stream"),
    )
}

/// A fill's extent seen from block 0. It refuses any write outside the extent and records the blocks written, so the
/// commit can refuse a fill that left any of the card's old bytes inside it.
pub(crate) struct Window<'a> {
    inner: &'a mut dyn BlockTarget,
    start: u64,
    written: Bitmap,
}

impl<'a> Window<'a> {
    pub(crate) fn new(inner: &'a mut dyn BlockTarget, start: u64, blocks: u64) -> Self {
        Self {
            inner,
            start,
            written: Bitmap::new(blocks),
        }
    }

    pub(crate) fn unwritten(&self) -> u64 {
        self.written.len() - self.written.count_set(0..self.written.len())
    }
}

impl BlockTarget for Window<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        let end = paddr
            .checked_add((data.len() / BLOCK_SIZE) as u64)
            .filter(|&end| end <= self.written.len() && data.len().is_multiple_of(BLOCK_SIZE));
        let Some(end) = end else {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "a write of {} bytes at block {paddr} of a {}-block extent",
                    data.len(),
                    self.written.len()
                ),
            ));
        };
        self.inner.write_blocks(self.start + paddr, data)?;
        self.written.set_range(paddr..end);
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}
