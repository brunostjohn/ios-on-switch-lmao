// SPDX-License-Identifier: GPL-3.0-or-later
//! The one checkpoint the writer commits: the ephemeral objects in the data area, then the checkpoint map and the
//! container superblock in the descriptor area.

use std::ops::Range;

use switch_apfs_btree::{TreeShape, leaf_root};
use switch_apfs_format::container::{
    CHECKPOINT_MAP_LAST, CPM_ENTRIES_AT, CPM_PER_BLOCK, CheckpointMapHeader, CheckpointMapping, NxSuperblock,
};
use switch_apfs_format::object::{
    OBJ_PHYSICAL, OBJECT_TYPE_MASK, ObjPhys, TYPE_BTREE, TYPE_BTREE_NODE, TYPE_CHECKPOINT_MAP, TYPE_NX_REAP_LIST,
    TYPE_NX_REAPER, TYPE_SPACEMAN, TYPE_SPACEMAN_FREE_QUEUE,
};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal};
use switch_apfs_read::Checkpoint;
use zerocopy::little_endian::{U32, U64};

use crate::WriteError;

pub struct Committed {
    pub ephemeral: Vec<(u64, Box<Block>)>,
    pub map: (u64, Box<Block>),
    pub superblock: Box<Block>,
    pub superblock_paddr: u64,
}

pub struct CheckpointInput<'a> {
    pub source: &'a Checkpoint,
    pub xid: u64,
    pub desc: Range<u64>,
    pub data: Range<u64>,
    pub spaceman: &'a Block,
    pub free_queue_oids: [u64; 2],
    pub nx: NxSuperblock,
}

/// The source's ephemeral objects in its own order: a new space manager, empty free-queue roots (every queued block
/// was freed into the bitmaps), and the reaper and reap list as Apple left them. Free-queue child nodes go with their
/// entries; anything else is refused.
pub fn build(input: CheckpointInput) -> Result<Committed, WriteError> {
    let xid = input.xid;
    let mut ephemeral = Vec::new();
    let mut mappings = Vec::new();
    for m in &input.source.mappings {
        let kind = m.type_and_flags.get() & OBJECT_TYPE_MASK;
        let subtype = m.subtype.get();
        let oid = m.oid.get();
        let block: Box<Block> = match kind {
            TYPE_SPACEMAN => Box::new(*input.spaceman),
            TYPE_BTREE if subtype == TYPE_SPACEMAN_FREE_QUEUE && input.free_queue_oids.contains(&oid) => {
                leaf_root(&TreeShape::FREE_QUEUE, oid, xid, &[])?
            }
            TYPE_BTREE_NODE if subtype == TYPE_SPACEMAN_FREE_QUEUE => continue,
            TYPE_NX_REAPER | TYPE_NX_REAP_LIST => {
                let (_, source) = input.source.ephemeral(oid)?;
                let mut block = source.clone();
                put(&mut block[..], 16, &U64::new(xid));
                seal(&mut block[..]);
                block
            }
            _ => {
                return Err(WriteError::Refused(format!(
                    "ephemeral object {oid} of type {kind:#x}/{subtype:#x}"
                )));
            }
        };
        let paddr = input.data.start + ephemeral.len() as u64;
        if paddr >= input.data.end {
            return Err(WriteError::Refused("the checkpoint data area is too small".into()));
        }
        mappings.push(CheckpointMapping {
            type_and_flags: m.type_and_flags,
            subtype: m.subtype,
            size: U32::new(BLOCK_SIZE as u32),
            pad: U32::ZERO,
            fs_oid: U64::ZERO,
            oid: m.oid,
            paddr: U64::new(paddr),
        });
        ephemeral.push((paddr, block));
    }
    if mappings.len() > CPM_PER_BLOCK || input.desc.end - input.desc.start < 2 {
        return Err(WriteError::Refused("the checkpoint doesn't fit one map block".into()));
    }

    let map_paddr = input.desc.start;
    let mut map = Box::new([0u8; BLOCK_SIZE]);
    let header = CheckpointMapHeader {
        o: ObjPhys::new(map_paddr, xid, OBJ_PHYSICAL | TYPE_CHECKPOINT_MAP, 0),
        flags: U32::new(CHECKPOINT_MAP_LAST),
        count: U32::new(mappings.len() as u32),
    };
    put(&mut map[..], 0, &header);
    for (k, m) in mappings.iter().enumerate() {
        put(&mut map[..], CPM_ENTRIES_AT + k * size_of::<CheckpointMapping>(), m);
    }
    seal(&mut map[..]);

    let mut nx = input.nx;
    nx.o.xid = U64::new(xid);
    nx.next_xid = U64::new(xid + 1);
    nx.xp_desc_blocks = U32::new((input.desc.end - input.desc.start) as u32);
    nx.xp_data_blocks = U32::new((input.data.end - input.data.start) as u32);
    nx.xp_desc_base = U64::new(input.desc.start);
    nx.xp_data_base = U64::new(input.data.start);
    nx.xp_desc_index = U32::ZERO;
    nx.xp_desc_len = U32::new(2);
    nx.xp_desc_next = U32::new(2);
    nx.xp_data_index = U32::ZERO;
    nx.xp_data_len = U32::new(ephemeral.len() as u32);
    nx.xp_data_next = U32::new(ephemeral.len() as u32);
    let mut superblock = Box::new(*input.source.nx_block);
    put(&mut superblock[..], 0, &nx);
    seal(&mut superblock[..]);
    Ok(Committed {
        ephemeral,
        map: (map_paddr, map),
        superblock,
        superblock_paddr: map_paddr + 1,
    })
}
