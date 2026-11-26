// SPDX-License-Identifier: GPL-3.0-or-later
//! The space-manager sizes a container must have for its block count. Ported from boot/installer/apfs.c
//! (GPL-2.0-or-later), whose rules were measured from newfs_apfs and the macOS 27 kernel; newfs_apfs's own containers
//! from 64 MiB to 1 TiB on macOS 27.2 match them.

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::spaceman::CHUNK_INFOS_PER_CIB;

pub const BLOCKS_PER_CHUNK: u64 = BLOCK_SIZE as u64 * 8;
pub const CHUNKS_PER_CIB: u64 = CHUNK_INFOS_PER_CIB as u64;
pub const CIBS_PER_CAB: u64 = ((BLOCK_SIZE - 40) / 8) as u64;
pub const POOL_BLOCKS_PER_CHUNK_INFO: u64 = 3;
pub const POOL_BITMAP_BLOCKS: u64 = 16;
pub const CIB_ADDRS_AT: usize = 0xA08;
const FREE_QUEUE_ENTRIES_PER_NODE: u64 = 1136;
const FREE_QUEUE_NODE_MAX: u64 = 512;
const MAX_FILE_SYSTEM_BYTES: u64 = 512 << 20;
const MAX_FILE_SYSTEMS: u64 = 100;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    pub blocks: u64,
    pub chunks: u64,
    pub cibs: u64,
    pub pool_blocks: u64,
    pub pool_queue_limit: u16,
    pub main_queue_limit: u16,
    pub max_file_systems: u32,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum GeometryError {
    #[error("{0} blocks need {1} internal-pool blocks, more than one pool bitmap block covers")]
    PoolTooLarge(u64, u64),
    #[error("{0} blocks need {1} chunk-info blocks, more than the space manager holds inline")]
    TooManyCibs(u64, u64),
    #[error("{0} blocks is too small for a container")]
    TooSmall(u64),
}

impl Geometry {
    pub fn for_blocks(blocks: u64) -> Result<Self, GeometryError> {
        if blocks < BLOCKS_PER_CHUNK / 8 {
            return Err(GeometryError::TooSmall(blocks));
        }
        let chunks = blocks.div_ceil(BLOCKS_PER_CHUNK);
        let cibs = chunks.div_ceil(CHUNKS_PER_CIB);
        let pool_blocks = POOL_BLOCKS_PER_CHUNK_INFO * (chunks + cibs);
        if pool_blocks > BLOCK_SIZE as u64 * 8 {
            return Err(GeometryError::PoolTooLarge(blocks, pool_blocks));
        }
        if CIB_ADDRS_AT as u64 + 8 * cibs > BLOCK_SIZE as u64 {
            return Err(GeometryError::TooManyCibs(blocks, cibs));
        }
        Ok(Self {
            blocks,
            chunks,
            cibs,
            pool_blocks,
            pool_queue_limit: pool_queue_limit(pool_blocks),
            main_queue_limit: main_queue_limit(blocks),
            max_file_systems: max_file_systems(blocks),
        })
    }

    pub fn chunk_blocks(&self, chunk: u64) -> u64 {
        BLOCKS_PER_CHUNK.min(self.blocks - chunk * BLOCKS_PER_CHUNK)
    }
}

/// One node, or at least a root over two leaves.
fn queue_limit(nodes: u64) -> u16 {
    if nodes <= 1 { 1 } else { nodes.max(3) as u16 }
}

pub fn pool_queue_limit(pool_blocks: u64) -> u16 {
    queue_limit(pool_blocks.div_ceil(FREE_QUEUE_ENTRIES_PER_NODE))
}

pub fn main_queue_limit(blocks: u64) -> u16 {
    let m = if blocks < 1 << 18 {
        1
    } else if blocks < 1 << 20 {
        2
    } else {
        4
    };
    queue_limit(
        (blocks * m)
            .div_ceil(4 * FREE_QUEUE_ENTRIES_PER_NODE)
            .min(FREE_QUEUE_NODE_MAX),
    )
}

pub fn max_file_systems(blocks: u64) -> u32 {
    (blocks * BLOCK_SIZE as u64)
        .div_ceil(MAX_FILE_SYSTEM_BYTES)
        .min(MAX_FILE_SYSTEMS) as u32
}
