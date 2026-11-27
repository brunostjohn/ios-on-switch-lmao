// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::{self, Cursor, Read};

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, BlockTarget};

use crate::layout::SealPlan;
use crate::place::DataWrite;
use crate::{ReplacementData, SealError};

const BS: u64 = BLOCK_SIZE as u64;
const CHUNK_BLOCKS: u64 = 2048;

/// Writes a standalone container image, zeroing block 0 first and committing block 0 last.
pub fn write_standalone(src: &dyn BlockSource, plan: &SealPlan, target: &mut dyn BlockTarget) -> Result<(), SealError> {
    let zero_block = [0u8; BLOCK_SIZE];
    target.write_blocks(0, &zero_block)?;
    target.flush()?;
    write_body(src, plan, target)?;
    target.flush()?;
    target.write_blocks(0, &plan.block_zero[..])?;
    target.flush()?;
    Ok(())
}

/// Writes every block in `0..total_blocks` sequentially for `switch_apfs_write::Fill`.
pub fn write_sequential(src: &dyn BlockSource, plan: &SealPlan, target: &mut dyn BlockTarget) -> Result<(), SealError> {
    target.write_blocks(0, &plan.block_zero[..])?;
    write_body(src, plan, target)?;
    Ok(())
}

fn write_body(src: &dyn BlockSource, plan: &SealPlan, target: &mut dyn BlockTarget) -> Result<(), SealError> {
    let mut batch = Batch::new(CHUNK_BLOCKS);
    let zero_block = [0u8; BLOCK_SIZE];
    let mut at = 1u64;
    for (paddr, block) in &plan.metadata {
        while at < *paddr {
            batch.put(target, at, &zero_block)?;
            at += 1;
        }
        batch.put(target, *paddr, &block[..])?;
        at = *paddr + 1;
    }
    while at < plan.data_start {
        batch.put(target, at, &zero_block)?;
        at += 1;
    }

    let mut copy_buf = vec![0u8; (CHUNK_BLOCKS * BS) as usize];
    for w in &plan.writes {
        match w {
            DataWrite::SourceCopy {
                src_paddr,
                dst_paddr,
                blocks,
            } => {
                let mut left = *blocks;
                let mut src_blk = *src_paddr;
                let mut dst = *dst_paddr;
                while left > 0 {
                    let n = left.min(CHUNK_BLOCKS);
                    let bytes = &mut copy_buf[..(n * BS) as usize];
                    src.read_blocks(src_blk, bytes)?;
                    batch.put(target, dst, bytes)?;
                    src_blk += n;
                    dst += n;
                    left -= n;
                }
            }
            DataWrite::Replacement {
                dst_paddr,
                blocks,
                data,
                size,
                sha256,
            } => {
                let written = write_replacement(data, *size, sha256, *dst_paddr, &mut copy_buf, &mut batch, target)?;
                if written != *blocks {
                    return Err(SealError::InvalidSpec("stream write length mismatch".into()));
                }
            }
        }
    }

    let mut tail = plan.used_end;
    if tail < plan.total_blocks {
        batch.finish(target)?;
        copy_buf.fill(0);
        while tail < plan.total_blocks {
            let n = (plan.total_blocks - tail).min(CHUNK_BLOCKS);
            target.write_blocks(tail, &copy_buf[..(n * BS) as usize])?;
            tail += n;
        }
    }
    batch.finish(target)?;
    Ok(())
}

fn write_replacement(
    data: &ReplacementData,
    expected_size: u64,
    expected_sha256: &[u8; 32],
    mut dst: u64,
    buf: &mut [u8],
    batch: &mut Batch,
    target: &mut dyn BlockTarget,
) -> Result<u64, SealError> {
    let mut reader: Box<dyn Read> = match data {
        ReplacementData::Bytes(bytes) => Box::new(Cursor::new(bytes.as_ref())),
        ReplacementData::File(path) => Box::new(File::open(path)?),
    };
    let mut hasher = Sha256::new();
    let mut total = 0u64;
    let start_dst = dst;
    loop {
        let mut filled = 0usize;
        while filled < buf.len() {
            match reader.read(&mut buf[filled..]) {
                Ok(0) => break,
                Ok(n) => filled += n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e.into()),
            }
        }
        if filled == 0 {
            break;
        }
        hasher.update(&buf[..filled]);
        total = total.checked_add(filled as u64).ok_or_else(|| {
            SealError::Io(io::Error::new(
                io::ErrorKind::InvalidData,
                "replacement byte count overflow",
            ))
        })?;
        if total > expected_size {
            return Err(SealError::Io(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("replacement grew past planned size {expected_size}"),
            )));
        }
        let blocks = filled.div_ceil(BLOCK_SIZE);
        let padded = blocks * BLOCK_SIZE;
        buf[filled..padded].fill(0);
        batch.put(target, dst, &buf[..padded])?;
        dst += blocks as u64;
        if filled < buf.len() {
            break;
        }
    }
    if total != expected_size {
        return Err(SealError::Io(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            format!("replacement produced {total} of {expected_size} bytes"),
        )));
    }
    let got: [u8; 32] = hasher.finalize().into();
    if &got != expected_sha256 {
        return Err(SealError::Io(io::Error::new(
            io::ErrorKind::InvalidData,
            "replacement bytes changed between plan and write",
        )));
    }
    Ok(dst - start_dst)
}

struct Batch {
    start: u64,
    buf: Vec<u8>,
    max: usize,
}

impl Batch {
    fn new(blocks: u64) -> Self {
        Self {
            start: 0,
            buf: Vec::new(),
            max: (blocks * BS) as usize,
        }
    }

    fn put(&mut self, out: &mut dyn BlockTarget, paddr: u64, data: &[u8]) -> io::Result<()> {
        let end = self.start + (self.buf.len() / BLOCK_SIZE) as u64;
        if !self.buf.is_empty() && (paddr != end || self.buf.len() + data.len() > self.max) {
            self.finish(out)?;
        }
        if self.buf.is_empty() {
            self.start = paddr;
        }
        if data.len() >= self.max {
            return out.write_blocks(paddr, data);
        }
        self.buf.extend_from_slice(data);
        Ok(())
    }

    fn finish(&mut self, out: &mut dyn BlockTarget) -> io::Result<()> {
        if !self.buf.is_empty() {
            out.write_blocks(self.start, &self.buf)?;
            self.buf.clear();
        }
        Ok(())
    }
}
