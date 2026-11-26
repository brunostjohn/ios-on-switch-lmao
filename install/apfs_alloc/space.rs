// SPDX-License-Identifier: GPL-3.0-or-later
//! The space manager, its chunk-info blocks, chunk bitmaps and internal-pool bitmap, for a final allocation map.
//! Field values follow Apple's own space managers: the 24B119 and 21H461 OS images', and newfs_apfs's on macOS 27.2.

use switch_apfs_format::object::{OBJ_EPHEMERAL, OBJ_PHYSICAL, ObjPhys, TYPE_SPACEMAN, TYPE_SPACEMAN_CIB};
use switch_apfs_format::spaceman::{
    CHUNK_INFO_AT, ChunkInfo, CibHeader, SD_MAIN, SFQ_IP, SFQ_MAIN, SM_FLAG_VERSIONED, SM_IP_BITMAP_RING_FREE_END,
    SPACEMAN_STRUCT_SIZE, SpacemanFreeQueue, SpacemanPhys,
};
use switch_apfs_format::{BLOCK_SIZE, Block, get, put, seal};
use zerocopy::FromZeros;
use zerocopy::little_endian::{U16, U32, U64};

use crate::bitmap::Bitmap;
use crate::geometry::{BLOCKS_PER_CHUNK, CHUNKS_PER_CIB, CIB_ADDRS_AT, CIBS_PER_CAB, Geometry, POOL_BITMAP_BLOCKS};

const IP_BM_TX_MULTIPLIER: u32 = 16;
const IP_BM_SIZE_IN_BLOCKS: u32 = 1;
/// newfs_apfs and the kernel keep the live pool bitmap in ring slot 1, with slots 2..15 then 0 free.
const IP_BITMAP_SLOT: u16 = 1;
const IP_BM_FREE_HEAD: u16 = 2;
const IP_BM_FREE_TAIL: u16 = 0;
const IP_BM_XID_AT: usize = SPACEMAN_STRUCT_SIZE;
const IP_BITMAP_AT: usize = IP_BM_XID_AT + 8;
const IP_BM_FREE_NEXT_AT: usize = IP_BITMAP_AT + 8;
const _: () = assert!(IP_BM_FREE_NEXT_AT + 2 * POOL_BITMAP_BLOCKS as usize == CIB_ADDRS_AT);

pub struct SpaceInput<'a> {
    pub geometry: &'a Geometry,
    /// Every block in use, the pool, its bitmap ring and the checkpoint areas included.
    pub used: &'a Bitmap,
    pub pool_base: u64,
    pub ring_base: u64,
    pub xid: u64,
    pub spaceman_oid: u64,
    pub free_queue_oids: [u64; 2],
}

pub struct Space {
    pub spaceman: Box<Block>,
    /// Chunk-info blocks, chunk bitmaps and the pool bitmap, each at its block.
    pub blocks: Vec<(u64, Box<Block>)>,
    pub free_blocks: u64,
    pub pool_used: u64,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum SpaceError {
    #[error("the internal pool [{0}, +{1}) or its bitmap ring isn't marked in use")]
    PoolNotReserved(u64, u64),
    #[error("{0} pool blocks needed, {1} in the pool")]
    PoolFull(u64, u64),
}

pub fn build(input: &SpaceInput) -> Result<Space, SpaceError> {
    let g = input.geometry;
    let pool = input.pool_base..input.pool_base + g.pool_blocks;
    let ring = input.ring_base..input.ring_base + POOL_BITMAP_BLOCKS;
    if !input.used.all_set(pool.clone()) || !input.used.all_set(ring.clone()) {
        return Err(SpaceError::PoolNotReserved(input.pool_base, g.pool_blocks));
    }
    let used_chunks: Vec<u64> = (0..g.chunks)
        .filter(|&c| !input.used.all_clear(chunk_range(g, c)))
        .collect();
    let pool_used = g.cibs + used_chunks.len() as u64;
    if pool_used > g.pool_blocks {
        return Err(SpaceError::PoolFull(pool_used, g.pool_blocks));
    }
    let cib_addr = |cib: u64| input.pool_base + cib;
    let mut bitmap_addr = vec![0u64; g.chunks as usize];
    let mut blocks = Vec::with_capacity(pool_used as usize + 1);
    for (n, &chunk) in used_chunks.iter().enumerate() {
        let addr = input.pool_base + g.cibs + n as u64;
        bitmap_addr[chunk as usize] = addr;
        let mut block = Box::new([0u8; BLOCK_SIZE]);
        input.used.to_bytes(chunk_range(g, chunk), &mut block[..]);
        blocks.push((addr, block));
    }

    let mut free_blocks = 0u64;
    for cib in 0..g.cibs {
        let first = cib * CHUNKS_PER_CIB;
        let chunks = first..(first + CHUNKS_PER_CIB).min(g.chunks);
        let mut block = Box::new([0u8; BLOCK_SIZE]);
        let header = CibHeader {
            o: ObjPhys::new(cib_addr(cib), input.xid, OBJ_PHYSICAL | TYPE_SPACEMAN_CIB, 0),
            index: U32::new(cib as u32),
            chunk_info_count: U32::new((chunks.end - chunks.start) as u32),
        };
        put(&mut block[..], 0, &header);
        for (n, chunk) in chunks.enumerate() {
            let range = chunk_range(g, chunk);
            let count = range.end - range.start;
            let free = count - input.used.count_set(range);
            free_blocks += free;
            let info = ChunkInfo {
                xid: U64::new(input.xid),
                addr: U64::new(chunk * BLOCKS_PER_CHUNK),
                block_count: U32::new(count as u32),
                free_count: U32::new(free as u32),
                bitmap_addr: U64::new(bitmap_addr[chunk as usize]),
            };
            put(&mut block[..], CHUNK_INFO_AT + n * size_of::<ChunkInfo>(), &info);
        }
        seal(&mut block[..]);
        blocks.push((cib_addr(cib), block));
    }

    let mut pool_bitmap = Box::new([0u8; BLOCK_SIZE]);
    for n in 0..pool_used {
        pool_bitmap[(n / 8) as usize] |= 1 << (n % 8);
    }
    blocks.push((input.ring_base + u64::from(IP_BITMAP_SLOT), pool_bitmap));

    let spaceman = spaceman_block(input, free_blocks);
    Ok(Space {
        spaceman,
        blocks,
        free_blocks,
        pool_used,
    })
}

fn chunk_range(g: &Geometry, chunk: u64) -> std::ops::Range<u64> {
    let start = chunk * BLOCKS_PER_CHUNK;
    start..start + g.chunk_blocks(chunk)
}

fn spaceman_block(input: &SpaceInput, free_blocks: u64) -> Box<Block> {
    let g = input.geometry;
    let mut sm = SpacemanPhys::new_zeroed();
    sm.o = ObjPhys::new(input.spaceman_oid, input.xid, OBJ_EPHEMERAL | TYPE_SPACEMAN, 0);
    sm.block_size = U32::new(BLOCK_SIZE as u32);
    sm.blocks_per_chunk = U32::new(BLOCKS_PER_CHUNK as u32);
    sm.chunks_per_cib = U32::new(CHUNKS_PER_CIB as u32);
    sm.cibs_per_cab = U32::new(CIBS_PER_CAB as u32);
    let main = &mut sm.dev[SD_MAIN];
    main.block_count = U64::new(g.blocks);
    main.chunk_count = U64::new(g.chunks);
    main.cib_count = U32::new(g.cibs as u32);
    main.free_count = U64::new(free_blocks);
    main.addr_offset = U32::new(CIB_ADDRS_AT as u32);
    sm.dev[1].addr_offset = U32::new((CIB_ADDRS_AT + 8 * g.cibs as usize) as u32);
    sm.flags = U32::new(SM_FLAG_VERSIONED);
    sm.ip_bm_tx_multiplier = U32::new(IP_BM_TX_MULTIPLIER);
    sm.ip_block_count = U64::new(g.pool_blocks);
    sm.ip_bm_size_in_blocks = U32::new(IP_BM_SIZE_IN_BLOCKS);
    sm.ip_bm_block_count = U32::new(POOL_BITMAP_BLOCKS as u32);
    sm.ip_bm_base = U64::new(input.ring_base);
    sm.ip_base = U64::new(input.pool_base);
    sm.fq[SFQ_IP] = empty_queue(input.free_queue_oids[0], g.pool_queue_limit);
    sm.fq[SFQ_MAIN] = empty_queue(input.free_queue_oids[1], g.main_queue_limit);
    sm.ip_bm_free_head = U16::new(IP_BM_FREE_HEAD);
    sm.ip_bm_free_tail = U16::new(IP_BM_FREE_TAIL);
    sm.ip_bm_xid_offset = U32::new(IP_BM_XID_AT as u32);
    sm.ip_bitmap_offset = U32::new(IP_BITMAP_AT as u32);
    sm.ip_bm_free_next_offset = U32::new(IP_BM_FREE_NEXT_AT as u32);
    sm.version = U32::new(1);
    sm.struct_size = U32::new(SPACEMAN_STRUCT_SIZE as u32);

    let mut block = Box::new([0u8; BLOCK_SIZE]);
    put(&mut block[..], 0, &sm);
    put(&mut block[..], IP_BM_XID_AT, &U64::new(input.xid));
    put(&mut block[..], IP_BITMAP_AT, &U16::new(IP_BITMAP_SLOT));
    for slot in 0..POOL_BITMAP_BLOCKS as u16 {
        let next = match slot {
            IP_BM_FREE_TAIL => SM_IP_BITMAP_RING_FREE_END,
            IP_BITMAP_SLOT => SM_IP_BITMAP_RING_FREE_END,
            s if s + 1 == POOL_BITMAP_BLOCKS as u16 => IP_BM_FREE_TAIL,
            s => s + 1,
        };
        put(
            &mut block[..],
            IP_BM_FREE_NEXT_AT + 2 * usize::from(slot),
            &U16::new(next),
        );
    }
    for cib in 0..g.cibs {
        put(
            &mut block[..],
            CIB_ADDRS_AT + 8 * cib as usize,
            &U64::new(input.pool_base + cib),
        );
    }
    seal(&mut block[..]);
    block
}

fn empty_queue(tree_oid: u64, limit: u16) -> SpacemanFreeQueue {
    SpacemanFreeQueue {
        tree_oid: U64::new(tree_oid),
        tree_node_limit: U16::new(limit),
        ..Default::default()
    }
}

/// The CIB addresses a space manager lists inline.
pub fn cib_addresses(spaceman: &[u8]) -> Vec<u64> {
    let Some(sm) = get::<SpacemanPhys>(spaceman, 0) else {
        return Vec::new();
    };
    let at = sm.dev[SD_MAIN].addr_offset.get() as usize;
    (0..sm.dev[SD_MAIN].cib_count.get() as usize)
        .filter_map(|i| get::<U64>(spaceman, at + 8 * i))
        .map(|a| a.get())
        .collect()
}
