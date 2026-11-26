// SPDX-License-Identifier: GPL-3.0-or-later
//! Targets for checking the commit order: one in memory, one that loses power after a number of writes, and the
//! scan for container superblocks a cut leaves behind.

use std::collections::BTreeMap;
use std::io;
use std::ops::Range;

use switch_apfs_format::container::{NX_MAGIC, NxSuperblock, XP_NONCONTIGUOUS};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_format::object::TYPE_NX_SUPERBLOCK;
use switch_apfs_format::{BLOCK_SIZE, Block, checksum_ok, get};

use crate::WriteError;

/// Written blocks only; the rest read as zeros.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct MemTarget {
    pub blocks: BTreeMap<u64, Box<Block>>,
}

impl BlockTarget for MemTarget {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        for (i, chunk) in data.chunks(BLOCK_SIZE).enumerate() {
            let mut block = Box::new([0u8; BLOCK_SIZE]);
            block[..chunk.len()].copy_from_slice(chunk);
            self.blocks.insert(paddr + i as u64, block);
        }
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl BlockSource for MemTarget {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
        for (i, chunk) in out.chunks_mut(BLOCK_SIZE).enumerate() {
            match self.blocks.get(&(paddr + i as u64)) {
                Some(b) => chunk.copy_from_slice(&b[..chunk.len()]),
                None => chunk.fill(0),
            }
        }
        Ok(())
    }
}

/// Loses power at its `writes_left`-th write: that write lands only its first half of blocks when `torn`, and every
/// later write or flush fails.
pub struct CutTarget<'a> {
    pub inner: &'a mut dyn BlockTarget,
    pub writes_left: u64,
    pub torn: bool,
    pub cut: bool,
}

impl<'a> CutTarget<'a> {
    pub fn new(inner: &'a mut dyn BlockTarget, writes_left: u64, torn: bool) -> Self {
        Self {
            inner,
            writes_left,
            torn,
            cut: false,
        }
    }
}

impl BlockTarget for CutTarget<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        if !self.cut && self.writes_left == 0 {
            self.cut = true;
            let blocks = data.len() / BLOCK_SIZE;
            if self.torn && blocks > 1 {
                self.inner.write_blocks(paddr, &data[..blocks / 2 * BLOCK_SIZE])?;
            }
        }
        if self.cut {
            return Err(io::Error::other("power cut"));
        }
        self.writes_left -= 1;
        self.inner.write_blocks(paddr, data)
    }

    fn flush(&mut self) -> io::Result<()> {
        if self.cut {
            return Err(io::Error::other("power cut"));
        }
        self.inner.flush()
    }
}

/// A block that holds a container superblock with a good checksum.
pub fn is_nxsb(block: &[u8]) -> bool {
    get::<NxSuperblock>(block, 0)
        .is_some_and(|nx| nx.magic.get() == NX_MAGIC && nx.o.object_type() == TYPE_NX_SUPERBLOCK && checksum_ok(block))
}

/// Every block holding a checksummed container superblock.
pub fn nxsb_blocks(mem: &MemTarget) -> Vec<u64> {
    mem.blocks
        .iter()
        .filter(|(_, b)| is_nxsb(&b[..]))
        .map(|(&p, _)| p)
        .collect()
}

/// The descriptor areas of whatever container block 0 of `target` names: that area, and the area of the newest
/// superblock in it when that one names another, as it does when block 0 lags behind. Empty when block 0 holds no
/// container. Phase B zeroes block 0 first, so a rerun can't read them again: read them once, before the first phase
/// B, keep them with the resume state, and give the same to every `plan`.
pub fn stale_descriptor_area(target: &dyn BlockSource) -> Result<Vec<Range<u64>>, WriteError> {
    let Some(named) = superblock(&target.block(0)?[..]) else {
        return Ok(Vec::new());
    };
    let area = desc_area(&named)?;
    let mut newest: Option<NxSuperblock> = None;
    let mut buf = vec![0u8; AREA_READ_BLOCKS as usize * BLOCK_SIZE];
    let mut at = area.start;
    while at < area.end {
        let n = (area.end - at).min(AREA_READ_BLOCKS);
        let bytes = &mut buf[..n as usize * BLOCK_SIZE];
        target.read_blocks(at, bytes)?;
        for nx in bytes.chunks(BLOCK_SIZE).filter_map(superblock) {
            if newest.is_none_or(|n| nx.o.xid.get() > n.o.xid.get()) {
                newest = Some(nx);
            }
        }
        at += n;
    }
    let mut areas = vec![area.clone()];
    if let Some(nx) = newest {
        let own = desc_area(&nx)?;
        if own != area {
            areas.push(own);
        }
    }
    Ok(areas)
}

const AREA_READ_BLOCKS: u64 = 2048;

fn superblock(block: &[u8]) -> Option<NxSuperblock> {
    is_nxsb(block).then(|| get(block, 0)).flatten()
}

fn desc_area(nx: &NxSuperblock) -> Result<Range<u64>, WriteError> {
    let blocks = nx.xp_desc_blocks.get();
    if blocks & XP_NONCONTIGUOUS != 0 {
        return Err(WriteError::Refused(
            "the old container's checkpoint descriptor area is a B-tree, not one run of blocks".into(),
        ));
    }
    let base = nx.xp_desc_base.get();
    Ok(base..base + u64::from(blocks))
}
