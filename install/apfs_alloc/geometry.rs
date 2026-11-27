// SPDX-License-Identifier: GPL-3.0-or-later
//! The space-manager and checkpoint sizes a container must have for its block count. The space-manager rules are
//! ported from boot/installer/apfs.c (GPL-2.0-or-later), whose rules were measured from newfs_apfs and the macOS 27
//! kernel. Every rule here equals what newfs_apfs and `diskutil apfs resizeContainer` give on macOS 27.2 for each of
//! the block counts in testdata/.

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::container::{CPM_ENTRIES_AT, CheckpointMapping};
use switch_apfs_format::spaceman::CHUNK_INFOS_PER_CIB;

pub const BLOCKS_PER_CHUNK: u64 = BLOCK_SIZE as u64 * 8;
pub const CHUNKS_PER_CIB: u64 = CHUNK_INFOS_PER_CIB as u64;
pub const CIBS_PER_CAB: u64 = ((BLOCK_SIZE - 40) / 8) as u64;
pub const POOL_BLOCKS_PER_CHUNK_INFO: u64 = 3;
pub const POOL_BITMAP_BLOCKS: u64 = 16;
pub const CIB_ADDRS_AT: usize = 0xA08;
/// 1 TiB, the largest size measured.
pub const MAX_BLOCKS: u64 = 1 << 28;
const MIN_BLOCKS: u64 = BLOCKS_PER_CHUNK / 8;
const FREE_QUEUE_ENTRIES_PER_NODE: u64 = 1136;
const FREE_QUEUE_NODE_MAX: u64 = 512;
const MAX_FILE_SYSTEM_BYTES: u64 = 512 << 20;
const MAX_FILE_SYSTEMS: u64 = 100;
const MAPPINGS_PER_CHECKPOINT_MAP: u64 = ((BLOCK_SIZE - CPM_ENTRIES_AT) / size_of::<CheckpointMapping>()) as u64;

const _: () = assert!(
    pool_blocks(MAX_BLOCKS) <= BLOCK_SIZE as u64 * 8,
    "one pool bitmap block covers the pool"
);
const _: () = assert!(
    CIB_ADDRS_AT as u64 + 8 * cibs(MAX_BLOCKS) <= BLOCK_SIZE as u64,
    "the spaceman lists every CIB inline, so no CABs"
);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    pub blocks: u64,
    pub chunks: u64,
    pub cibs: u64,
    pub pool_blocks: u64,
    pub pool_queue_limit: u16,
    pub main_queue_limit: u16,
    pub max_file_systems: u32,
    pub checkpoint: CheckpointSizes,
}

/// The checkpoint descriptor and data areas, in blocks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CheckpointSizes {
    pub desc_blocks: u32,
    pub data_blocks: u32,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum GeometryError {
    #[error("{0} blocks is larger than a container can be ({MAX_BLOCKS} blocks)")]
    TooLarge(u64),
    #[error("{0} blocks is too small for a container")]
    TooSmall(u64),
}

impl Geometry {
    pub fn for_blocks(blocks: u64) -> Result<Self, GeometryError> {
        if blocks < MIN_BLOCKS {
            return Err(GeometryError::TooSmall(blocks));
        }
        if blocks > MAX_BLOCKS {
            return Err(GeometryError::TooLarge(blocks));
        }
        let pool_blocks = pool_blocks(blocks);
        let pool_queue_limit = pool_queue_limit(pool_blocks);
        let main_queue_limit = main_queue_limit(blocks);
        let max_file_systems = max_file_systems(blocks);
        Ok(Self {
            blocks,
            chunks: chunks(blocks),
            cibs: cibs(blocks),
            pool_blocks,
            pool_queue_limit,
            main_queue_limit,
            max_file_systems,
            checkpoint: checkpoint_sizes(blocks, main_queue_limit, pool_queue_limit, max_file_systems),
        })
    }

    pub fn chunk_blocks(&self, chunk: u64) -> u64 {
        BLOCKS_PER_CHUNK.min(self.blocks - chunk * BLOCKS_PER_CHUNK)
    }
}

const fn chunks(blocks: u64) -> u64 {
    blocks.div_ceil(BLOCKS_PER_CHUNK)
}

const fn cibs(blocks: u64) -> u64 {
    chunks(blocks).div_ceil(CHUNKS_PER_CIB)
}

const fn pool_blocks(blocks: u64) -> u64 {
    POOL_BLOCKS_PER_CHUNK_INFO * (chunks(blocks) + cibs(blocks))
}

/// One node, or at least a root over two leaves.
fn queue_limit(nodes: u64) -> u16 {
    if nodes <= 1 { 1 } else { nodes.max(3) as u16 }
}

pub fn pool_queue_limit(pool_blocks: u64) -> u16 {
    queue_limit(pool_blocks.div_ceil(FREE_QUEUE_ENTRIES_PER_NODE))
}

/// The main queue's node count before the 512-node cap.
fn main_queue_nodes(blocks: u64) -> u64 {
    let m = if blocks < 1 << 18 {
        1
    } else if blocks < 1 << 20 {
        2
    } else {
        4
    };
    (blocks * m).div_ceil(4 * FREE_QUEUE_ENTRIES_PER_NODE)
}

pub fn main_queue_limit(blocks: u64) -> u16 {
    queue_limit(main_queue_nodes(blocks).min(FREE_QUEUE_NODE_MAX))
}

pub fn max_file_systems(blocks: u64) -> u32 {
    (blocks * BLOCK_SIZE as u64)
        .div_ceil(MAX_FILE_SYSTEM_BYTES)
        .min(MAX_FILE_SYSTEMS) as u32
}

/// Both areas follow from one count of objects: 64 per volume slot, or 8 per uncapped main-queue node when that is
/// fewer, plus both queue limits and 3. The data area is four times that count, and the descriptor area four times
/// the checkpoint-map blocks the count needs plus one. resizeContainer applies the same rule at the size it grows to,
/// and Apple's OS images carry this rule's areas for their own sizes.
fn checkpoint_sizes(blocks: u64, main_queue_limit: u16, pool_queue_limit: u16, max_fs: u32) -> CheckpointSizes {
    let objects = (64 * u64::from(max_fs)).min(8 * main_queue_nodes(blocks))
        + u64::from(main_queue_limit)
        + u64::from(pool_queue_limit)
        + 3;
    CheckpointSizes {
        desc_blocks: (4 * (objects.div_ceil(MAPPINGS_PER_CHECKPOINT_MAP) + 1)) as u32,
        data_blocks: (4 * objects) as u32,
    }
}
