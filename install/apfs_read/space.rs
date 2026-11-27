// SPDX-License-Identifier: GPL-3.0-or-later
//! The source's space manager: what its chunk bitmaps mark in use, where its internal pool and pool-bitmap ring
//! sit, and what its free queues hold.

use std::ops::Range;

use switch_apfs_alloc::Bitmap;
use switch_apfs_alloc::geometry::BLOCKS_PER_CHUNK;
use switch_apfs_alloc::space::cib_addresses;
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::{OBJ_PHYSICAL, TYPE_SPACEMAN_CIB};
use switch_apfs_format::spaceman::{
    CHUNK_INFO_AT, CI_COUNT_MASK, ChunkInfo, CibHeader, FreeQueueKey, SD_MAIN, SFQ_COUNT, SpacemanPhys,
};
use switch_apfs_format::{BLOCK_SIZE, Block, checksum_ok, get};

use crate::ReadError;
use crate::checkpoint::Checkpoint;
use crate::walk::{NodeReader, walk};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct QueuedFree {
    pub xid: u64,
    pub paddr: u64,
    pub len: u64,
}

pub struct Space {
    pub spaceman: SpacemanPhys,
    pub used: Bitmap,
    pub free_count: u64,
    pub pool: Range<u64>,
    pub pool_bitmap_ring: Range<u64>,
    pub cibs: Vec<u64>,
    pub chunk_bitmaps: Vec<u64>,
    /// Internal-pool queue, then main queue.
    pub queues: [Vec<QueuedFree>; 2],
    pub queue_nodes: [Vec<u64>; 2],
}

struct Ephemeral<'a>(&'a Checkpoint);

impl NodeReader for Ephemeral<'_> {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError> {
        let (paddr, block) = self.0.ephemeral(oid)?;
        Ok((*paddr, block.clone()))
    }
}

pub fn read(src: &dyn BlockSource, cp: &Checkpoint) -> Result<Space, ReadError> {
    let (_, sm_block) = cp.ephemeral(cp.nx.spaceman_oid.get())?;
    let sm: SpacemanPhys = get(&sm_block[..], 0).ok_or_else(|| ReadError::invalid("short space manager"))?;
    let dev = &sm.dev[SD_MAIN];
    let blocks = cp.nx.block_count.get();
    if sm.block_size.get() as usize != BLOCK_SIZE
        || u64::from(sm.blocks_per_chunk.get()) != BLOCKS_PER_CHUNK
        || dev.block_count.get() != blocks
        || dev.chunk_count.get() != blocks.div_ceil(BLOCKS_PER_CHUNK)
        || dev.cab_count.get() != 0
        || sm.dev[1].block_count.get() != 0
        || sm.fq[2].tree_oid.get() != 0
    {
        return Err(ReadError::Unsupported(
            "space manager shape (CABs, a second device, or other sizes)".into(),
        ));
    }
    let cpc = u64::from(sm.chunks_per_cib.get());
    let pool_base = sm.ip_base.get();
    let pool = pool_base..pool_base + sm.ip_block_count.get();
    let ring_base = sm.ip_bm_base.get();
    let pool_bitmap_ring = ring_base..ring_base + u64::from(sm.ip_bm_block_count.get());

    let cibs = cib_addresses(&sm_block[..]);
    if cibs.len() as u64 != u64::from(dev.cib_count.get()) {
        return Err(ReadError::invalid(
            "space manager lists the wrong number of chunk-info blocks",
        ));
    }
    let mut used = Bitmap::new(blocks);
    let mut chunk_bitmaps = Vec::new();
    let mut chunk = 0u64;
    for (index, &cib_addr) in cibs.iter().enumerate() {
        let block = src.block(cib_addr)?;
        let cib: CibHeader = get(&block[..], 0).unwrap_or_default();
        let count = u64::from(cib.chunk_info_count.get());
        if !checksum_ok(&block[..])
            || cib.o.type_and_flags.get() != OBJ_PHYSICAL | TYPE_SPACEMAN_CIB
            || cib.index.get() as usize != index
            || count > cpc
        {
            return Err(ReadError::invalid(format!("chunk-info block {index} at {cib_addr}")));
        }
        for n in 0..count as usize {
            let info: ChunkInfo = get(&block[..], CHUNK_INFO_AT + n * size_of::<ChunkInfo>()).unwrap_or_default();
            read_chunk(src, &info, chunk, &mut used, &mut chunk_bitmaps)?;
            chunk += 1;
        }
    }
    if chunk != dev.chunk_count.get() {
        return Err(ReadError::invalid(format!(
            "{chunk} chunks described, {} expected",
            dev.chunk_count.get()
        )));
    }
    let free_count = blocks - used.count_set(0..blocks);
    if free_count != dev.free_count.get() {
        return Err(ReadError::invalid(format!(
            "bitmaps free {free_count}, space manager says {}",
            dev.free_count.get()
        )));
    }
    for &addr in cibs.iter().chain(&chunk_bitmaps) {
        if !pool.contains(&addr) {
            return Err(ReadError::Unsupported(format!(
                "space-manager block {addr} outside the internal pool"
            )));
        }
    }

    let mut queues = [Vec::new(), Vec::new()];
    let mut queue_nodes = [Vec::new(), Vec::new()];
    for q in 0..SFQ_COUNT - 1 {
        let tree = sm.fq[q].tree_oid.get();
        if tree == 0 {
            continue;
        }
        let entries = &mut queues[q];
        let visited = walk(&Ephemeral(cp), tree, &mut |key, value| {
            let key: FreeQueueKey = get(key, 0).ok_or_else(|| ReadError::invalid("short free-queue key"))?;
            let len = match value {
                Some(v) => u64::from_le_bytes(v.try_into().map_err(|_| ReadError::invalid("free-queue value"))?),
                None => 1,
            };
            entries.push(QueuedFree {
                xid: key.xid.get(),
                paddr: key.paddr.get(),
                len,
            });
            Ok(())
        })?;
        queue_nodes[q] = visited.nodes;
    }
    Ok(Space {
        spaceman: sm,
        used,
        free_count,
        pool,
        pool_bitmap_ring,
        cibs,
        chunk_bitmaps,
        queues,
        queue_nodes,
    })
}

fn read_chunk(
    src: &dyn BlockSource,
    info: &ChunkInfo,
    chunk: u64,
    used: &mut Bitmap,
    chunk_bitmaps: &mut Vec<u64>,
) -> Result<(), ReadError> {
    let start = chunk * BLOCKS_PER_CHUNK;
    let count = u64::from(info.block_count.get() & CI_COUNT_MASK);
    let free = u64::from(info.free_count.get());
    if info.addr.get() != start || start + count > used.len() || free > count {
        return Err(ReadError::invalid(format!("chunk {chunk} info")));
    }
    let bitmap_addr = info.bitmap_addr.get();
    if bitmap_addr == 0 {
        return if free == count {
            Ok(())
        } else {
            Err(ReadError::invalid(format!("chunk {chunk}: used, no bitmap")))
        };
    }
    let bits = src.block(bitmap_addr)?;
    used.set_from_bytes(start, count, &bits[..]);
    if used.count_set(start..start + count) != count - free {
        return Err(ReadError::invalid(format!(
            "chunk {chunk}: bitmap disagrees with its free count"
        )));
    }
    chunk_bitmaps.push(bitmap_addr);
    Ok(())
}
