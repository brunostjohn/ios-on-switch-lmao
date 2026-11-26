// SPDX-License-Identifier: GPL-3.0-or-later
//! The newest valid checkpoint, from the Reference's mount procedure: the descriptor area's NXSB with the highest
//! xid whose checkpoint maps are intact, and the ephemeral objects those maps place in the data area.

use std::collections::BTreeMap;
use std::ops::Range;

use switch_apfs_format::container::{
    CHECKPOINT_MAP_LAST, CPM_ENTRIES_AT, CheckpointMapHeader, CheckpointMapping, NX_INCOMPAT_VERSION2, NX_MAGIC,
    NxSuperblock, XP_NONCONTIGUOUS,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::{OBJ_EPHEMERAL, TYPE_CHECKPOINT_MAP, TYPE_NX_SUPERBLOCK};
use switch_apfs_format::{BLOCK_SIZE, Block, checksum_ok, get, header};

use crate::ReadError;

pub struct Checkpoint {
    pub nx: NxSuperblock,
    pub nx_block: Box<Block>,
    pub nx_paddr: u64,
    pub block_zero: Box<Block>,
    pub desc_area: Range<u64>,
    pub data_area: Range<u64>,
    pub mappings: Vec<CheckpointMapping>,
    pub ephemeral: BTreeMap<u64, (u64, Box<Block>)>,
}

impl Checkpoint {
    pub fn xid(&self) -> u64 {
        self.nx.o.xid.get()
    }

    pub fn ephemeral(&self, oid: u64) -> Result<&(u64, Box<Block>), ReadError> {
        self.ephemeral
            .get(&oid)
            .ok_or_else(|| ReadError::invalid(format!("no ephemeral object {oid} in the checkpoint")))
    }
}

fn superblock(block: &[u8]) -> Option<NxSuperblock> {
    let nx: NxSuperblock = get(block, 0)?;
    let ok = nx.magic.get() == NX_MAGIC
        && checksum_ok(block)
        && nx.o.object_type() == TYPE_NX_SUPERBLOCK
        && nx.block_size.get() as usize == BLOCK_SIZE;
    ok.then_some(nx)
}

pub fn read(src: &dyn BlockSource) -> Result<Checkpoint, ReadError> {
    let block_zero = src.block(0)?;
    let zero = superblock(&block_zero[..]).ok_or_else(|| ReadError::invalid("block 0 is not a 4 KiB NXSB"))?;
    if zero.incompatible_features.get() != NX_INCOMPAT_VERSION2 || zero.features.get() != 0 {
        return Err(ReadError::Unsupported(format!(
            "container features {:#x}, incompatible {:#x}",
            zero.features.get(),
            zero.incompatible_features.get()
        )));
    }
    let desc_blocks = zero.xp_desc_blocks.get();
    let data_blocks = zero.xp_data_blocks.get();
    if (desc_blocks | data_blocks) & XP_NONCONTIGUOUS != 0 {
        return Err(ReadError::Unsupported("non-contiguous checkpoint areas".into()));
    }
    let desc_base = zero.xp_desc_base.get();
    let data_base = zero.xp_data_base.get();
    let desc_area = desc_base..desc_base + u64::from(desc_blocks);
    let data_area = data_base..data_base + u64::from(data_blocks);

    let mut candidates = Vec::new();
    for paddr in desc_area.clone() {
        let block = src.block(paddr)?;
        if let Some(nx) = superblock(&block[..]) {
            candidates.push((nx.o.xid.get(), paddr, nx, block));
        }
    }
    candidates.sort_by_key(|c| std::cmp::Reverse(c.0));
    let mut last_error = ReadError::invalid("no valid checkpoint in the descriptor area");
    for (_, nx_paddr, nx, nx_block) in candidates {
        match mappings(src, &nx, &desc_area, &data_area) {
            Ok((mappings, ephemeral)) => {
                return Ok(Checkpoint {
                    nx,
                    nx_block,
                    nx_paddr,
                    block_zero,
                    desc_area,
                    data_area,
                    mappings,
                    ephemeral,
                });
            }
            Err(e) => last_error = e,
        }
    }
    Err(last_error)
}

#[allow(clippy::type_complexity)]
fn mappings(
    src: &dyn BlockSource,
    nx: &NxSuperblock,
    desc: &Range<u64>,
    data: &Range<u64>,
) -> Result<(Vec<CheckpointMapping>, BTreeMap<u64, (u64, Box<Block>)>), ReadError> {
    let xid = nx.o.xid.get();
    let desc_count = desc.end - desc.start;
    let maps = u64::from(nx.xp_desc_len.get())
        .checked_sub(1)
        .ok_or_else(|| ReadError::invalid("empty checkpoint"))?;
    let mut out: Vec<CheckpointMapping> = Vec::new();
    for j in 0..maps {
        let paddr = desc.start + (u64::from(nx.xp_desc_index.get()) + j) % desc_count;
        let block = src.block(paddr)?;
        let map: CheckpointMapHeader = get(&block[..], 0).unwrap_or_default();
        if !checksum_ok(&block[..]) || map.o.object_type() != TYPE_CHECKPOINT_MAP || map.o.xid.get() != xid {
            return Err(ReadError::invalid(format!("checkpoint {xid}: bad map at {paddr}")));
        }
        for k in 0..map.count.get() as usize {
            out.push(get(&block[..], CPM_ENTRIES_AT + k * size_of::<CheckpointMapping>()).unwrap_or_default());
        }
        if (map.flags.get() & CHECKPOINT_MAP_LAST != 0) != (j + 1 == maps) {
            return Err(ReadError::invalid(format!(
                "checkpoint {xid}: CHECKPOINT_MAP_LAST out of place"
            )));
        }
    }
    let mut ephemeral = BTreeMap::new();
    for m in &out {
        let paddr = m.paddr.get();
        if m.size.get() as usize != BLOCK_SIZE || !data.contains(&paddr) {
            return Err(ReadError::Unsupported(format!(
                "ephemeral object {} of {} bytes",
                m.oid.get(),
                m.size.get()
            )));
        }
        let block = src.block(paddr)?;
        let h = header(&block[..]);
        if !checksum_ok(&block[..])
            || h.oid.get() != m.oid.get()
            || h.xid.get() != xid
            || h.type_and_flags.get() != m.type_and_flags.get()
            || h.storage() != OBJ_EPHEMERAL
        {
            return Err(ReadError::invalid(format!(
                "checkpoint {xid}: ephemeral object {} at {paddr}",
                m.oid.get()
            )));
        }
        ephemeral.insert(m.oid.get(), (paddr, block));
    }
    Ok((out, ephemeral))
}
