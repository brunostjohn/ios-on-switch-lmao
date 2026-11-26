// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_alloc::geometry::{BLOCKS_PER_CHUNK, POOL_BITMAP_BLOCKS};
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{Layout, Record, TreeShape, leaf_root};
use switch_apfs_format::fs_records::{APFS_HASH_SHA256_SIZE, FextKey};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_format::omap::{OMAP_MANUALLY_MANAGED, OMAP_VAL_NOHEADER};
use switch_apfs_format::volume::{APFS_MODIFIED_NAMELEN, APFS_VOLNAME_LEN};
use switch_apfs_format::{BLOCK_SIZE, Block, get};
use switch_apfs_write::extents::{self, Owned};
use switch_apfs_write::volume::omap_block;

use crate::blocks::{
    FQ_IP_OID, FQ_MAIN_OID, FS_ROOT_OID, INTEGRITY_OID, SEAL_XID, SPACEMAN_OID, VOL_OID, VolumeCounts, VolumeRefs,
    checkpoint_blocks, emit_physical, integrity_block, nx_superblock, omap_entry, volume_superblock,
};
use crate::place::{DataWrite, encode_fext, place_relative_data};
use crate::records::BuiltRecords;
use crate::source::SourceCryptex;
use crate::{SealError, SealOptions, write};

const BS: u64 = BLOCK_SIZE as u64;
const MIN_CONTAINER_BLOCKS: u64 = BLOCKS_PER_CHUNK / 8;

#[derive(Clone, Debug)]
pub struct SealStats {
    pub total_blocks: u64,
    pub total_bytes: u64,
    pub free_blocks: u64,
    pub data_blocks: u64,
    pub metadata_blocks: u64,
    pub fs_nodes: usize,
    pub fs_levels: u16,
    pub fext_nodes: usize,
    pub extentref_nodes: usize,
    pub omap_nodes: usize,
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub other: u64,
    pub replaced_inodes: u64,
    pub root_hash: [u8; APFS_HASH_SHA256_SIZE],
    pub volume_uuid: [u8; 16],
    pub volume_name: String,
}

pub struct SealPlan {
    pub(crate) total_blocks: u64,
    pub(crate) data_start: u64,
    pub(crate) used_end: u64,
    pub(crate) block_zero: Box<Block>,
    pub(crate) metadata: Vec<(u64, Box<Block>)>,
    pub(crate) writes: Vec<DataWrite>,
    pub(crate) stats: SealStats,
}

impl SealPlan {
    pub fn exact_blocks(&self) -> u64 {
        self.total_blocks
    }

    pub fn exact_size(&self) -> u64 {
        self.stats.total_bytes
    }

    pub fn root_hash(&self) -> [u8; APFS_HASH_SHA256_SIZE] {
        self.stats.root_hash
    }

    pub fn stats(&self) -> &SealStats {
        &self.stats
    }

    pub fn write(&self, src: &dyn BlockSource, target: &mut dyn BlockTarget) -> Result<(), SealError> {
        write::write_standalone(src, self, target)
    }
}

pub fn build(
    cryptex: &SourceCryptex,
    records: BuiltRecords,
    replaced_inodes: u64,
    options: &SealOptions,
) -> Result<SealPlan, SealError> {
    let volume_name = options.volume_name.clone().unwrap_or_else(|| cryptex.sb.name());
    if volume_name.is_empty() || volume_name.len() >= APFS_VOLNAME_LEN || volume_name.contains('\0') {
        return Err(SealError::InvalidSpec("invalid volume name".into()));
    }
    if options.formatted_by.len() >= APFS_MODIFIED_NAMELEN || options.formatted_by.contains('\0') {
        return Err(SealError::InvalidSpec("invalid formatted_by".into()));
    }

    let placed = place_relative_data(records.streams)?;
    let counts = VolumeCounts {
        next_obj_id: records.next_obj_id,
        files: records.files,
        dirs: records.dirs,
        symlinks: records.symlinks,
        other: records.other,
        data_blocks: placed.unique_blocks,
    };

    let fs_layout = Layout::new(&TreeShape::SEALED_FS_TREE, &records.fs)?;
    let fs_nodes = fs_layout.node_count();
    let fs_levels = fs_layout.height();

    let omap_placeholder: Vec<Record> = (0..1 + fs_nodes)
        .map(|i| omap_entry(INTEGRITY_OID + i as u64, SEAL_XID, 0, 0))
        .collect();
    let omap_nodes = Layout::new(&TreeShape::OMAP, &omap_placeholder)?.node_count();

    let fext_placeholder: Vec<Record> = (0..placed.fext_count)
        .map(|i| encode_fext(i as u64, 0, BS, 0, 0))
        .collect();
    let fext_nodes = Layout::new(&TreeShape::FEXT, &fext_placeholder)?.node_count();

    let er_preview = extents::build(&placed.rel_owned);
    if er_preview.blocks != placed.unique_blocks {
        return Err(SealError::InvalidSpec(format!(
            "extentref block count {} disagrees with unique data blocks {}",
            er_preview.blocks, placed.unique_blocks
        )));
    }
    let er_nodes = Layout::new(&TreeShape::EXTENT_REF, &er_preview.records)?.node_count();

    let volume_metadata_blocks =
        1u64 + 1 + omap_nodes as u64 + 1 + 1 + fs_nodes as u64 + fext_nodes as u64 + er_nodes as u64;
    let volume_blocks = volume_metadata_blocks
        .checked_add(counts.data_blocks)
        .ok_or_else(|| SealError::InvalidSpec("volume block count overflow".into()))?;
    let (total_blocks, g) = exact_container_geometry(volume_blocks)?;
    let total_bytes = total_blocks
        .checked_mul(BS)
        .ok_or_else(|| SealError::InvalidSpec("container byte size overflow".into()))?;

    let mut cur = 1u64;
    let desc_base = cur;
    cur += u64::from(g.checkpoint.desc_blocks);
    let data_base = cur;
    cur += u64::from(g.checkpoint.data_blocks);
    let ring_base = cur;
    cur += POOL_BITMAP_BLOCKS;
    let pool_base = cur;
    cur += g.pool_blocks;
    let nx_omap_paddr = cur;
    let nx_omap_tree_paddr = cur + 1;
    cur += 2;

    let vol_sb_paddr = cur;
    let vol_omap_paddr = cur + 1;
    cur += 2;
    let vol_omap_addrs: Vec<u64> = (cur..cur + omap_nodes as u64).collect();
    cur += omap_nodes as u64;
    let integrity_paddr = cur;
    let snap_meta_paddr = cur + 1;
    cur += 2;
    let fs_addrs: Vec<u64> = (cur..cur + fs_nodes as u64).collect();
    cur += fs_nodes as u64;
    let fext_addrs: Vec<u64> = (cur..cur + fext_nodes as u64).collect();
    cur += fext_nodes as u64;
    let er_addrs: Vec<u64> = (cur..cur + er_nodes as u64).collect();
    cur += er_nodes as u64;
    let data_start = cur;

    let used_end = data_start
        .checked_add(placed.unique_blocks)
        .ok_or_else(|| SealError::InvalidSpec("data end overflow".into()))?;
    if used_end > total_blocks {
        return Err(SealError::InvalidSpec(format!(
            "planned {used_end} blocks exceed container size {total_blocks}"
        )));
    }

    let mut fext_records = Vec::with_capacity(placed.fext_count);
    for s in &placed.stream_runs {
        for r in &s.runs {
            fext_records.push(encode_fext(
                s.stream_id,
                r.logical,
                r.len,
                r.flags,
                data_start + r.rel_dst,
            ));
        }
    }
    let owned: Vec<Owned> = placed
        .rel_owned
        .iter()
        .map(|o| Owned {
            paddr: data_start + o.paddr,
            blocks: o.blocks,
            owner: o.owner,
        })
        .collect();
    let mut writes = Vec::with_capacity(placed.src_runs.len() + placed.replacements.len());
    for (src_paddr, blocks, rel_dst) in placed.src_runs {
        writes.push(DataWrite::SourceCopy {
            src_paddr,
            dst_paddr: data_start + rel_dst,
            blocks,
        });
    }
    for r in placed.replacements {
        writes.push(DataWrite::Replacement {
            dst_paddr: data_start + r.rel_dst,
            blocks: r.blocks,
            data: r.data,
            size: r.size,
            sha256: r.sha256,
        });
    }

    let mut next_fs_oid = FS_ROOT_OID;
    let fs_built = fs_layout.emit(SEAL_XID, &mut || {
        let id = next_fs_oid;
        next_fs_oid += 1;
        id
    });
    let root_hash = fs_built
        .root_hash
        .ok_or_else(|| SealError::InvalidSpec("sealed fs tree produced no root hash".into()))?;
    let nx_next_oid = next_fs_oid;

    let mut vol_omap_records = Vec::with_capacity(1 + fs_nodes);
    vol_omap_records.push(omap_entry(INTEGRITY_OID, SEAL_XID, 0, integrity_paddr));
    let mut metadata: Vec<(u64, Box<Block>)> = Vec::new();
    for (node, &paddr) in fs_built.nodes.into_iter().zip(&fs_addrs) {
        vol_omap_records.push(omap_entry(node.id, SEAL_XID, OMAP_VAL_NOHEADER, paddr));
        metadata.push((paddr, node.block));
    }

    let vol_omap_layout = Layout::new(&TreeShape::OMAP, &vol_omap_records)?;
    if vol_omap_layout.node_count() != omap_nodes {
        return Err(SealError::InvalidSpec("volume omap node count changed".into()));
    }
    metadata.extend(emit_physical(&vol_omap_layout, SEAL_XID, &vol_omap_addrs));
    metadata.push((
        vol_omap_paddr,
        omap_block(vol_omap_paddr, vol_omap_addrs[0], SEAL_XID, 0),
    ));
    metadata.push((integrity_paddr, integrity_block(INTEGRITY_OID, SEAL_XID, &root_hash)));
    metadata.push((
        snap_meta_paddr,
        leaf_root(&TreeShape::SNAP_META, snap_meta_paddr, SEAL_XID, &[])?,
    ));

    fext_records.sort_by_key(|(k, _)| {
        let fk: FextKey = get(k, 0).unwrap_or_default();
        (fk.private_id.get(), fk.logical_addr.get())
    });
    let fext_layout = Layout::new(&TreeShape::FEXT, &fext_records)?;
    if fext_layout.node_count() != fext_nodes {
        return Err(SealError::InvalidSpec("fext tree node count changed".into()));
    }
    metadata.extend(emit_physical(&fext_layout, SEAL_XID, &fext_addrs));

    let er_built = extents::build(&owned);
    let er_layout = Layout::new(&TreeShape::EXTENT_REF, &er_built.records)?;
    if er_layout.node_count() != er_nodes {
        return Err(SealError::InvalidSpec("extentref tree node count changed".into()));
    }
    metadata.extend(emit_physical(&er_layout, SEAL_XID, &er_addrs));

    let vsb = volume_superblock(
        cryptex,
        options,
        &volume_name,
        &counts,
        VolumeRefs {
            omap: vol_omap_paddr,
            root_tree_oid: FS_ROOT_OID,
            extentref: er_addrs[0],
            snap_meta: snap_meta_paddr,
            integrity_oid: INTEGRITY_OID,
            fext: fext_addrs[0],
            fs_alloc_count: volume_metadata_blocks - 1 + counts.data_blocks,
        },
    );
    metadata.push((vol_sb_paddr, vsb));

    let nx_omap_records = vec![omap_entry(VOL_OID, SEAL_XID, 0, vol_sb_paddr)];
    metadata.push((
        nx_omap_tree_paddr,
        leaf_root(&TreeShape::OMAP, nx_omap_tree_paddr, SEAL_XID, &nx_omap_records)?,
    ));
    metadata.push((
        nx_omap_paddr,
        omap_block(nx_omap_paddr, nx_omap_tree_paddr, SEAL_XID, OMAP_MANUALLY_MANAGED),
    ));

    let mut used = Bitmap::new(total_blocks);
    used.set_range(0..used_end);
    let space = space::build(&SpaceInput {
        geometry: &g,
        used: &used,
        pool_base,
        ring_base,
        xid: SEAL_XID,
        spaceman_oid: SPACEMAN_OID,
        free_queue_oids: [FQ_IP_OID, FQ_MAIN_OID],
    })?;
    metadata.extend(space.blocks);
    metadata.extend(checkpoint_blocks(desc_base, data_base, SEAL_XID, space.spaceman)?);

    let nx_block = nx_superblock(
        cryptex,
        &g,
        options.volume_uuid,
        nx_next_oid,
        desc_base,
        data_base,
        nx_omap_paddr,
    );
    metadata.push((desc_base + 1, nx_block.clone()));

    metadata.sort_by_key(|(paddr, _)| *paddr);
    for w in metadata.windows(2) {
        if w[0].0 == w[1].0 {
            return Err(SealError::InvalidSpec(format!("duplicate metadata block {}", w[0].0)));
        }
    }

    let stats = SealStats {
        total_blocks,
        total_bytes,
        free_blocks: space.free_blocks,
        data_blocks: counts.data_blocks,
        metadata_blocks: data_start,
        fs_nodes,
        fs_levels,
        fext_nodes,
        extentref_nodes: er_nodes,
        omap_nodes,
        files: counts.files,
        dirs: counts.dirs,
        symlinks: counts.symlinks,
        other: counts.other,
        replaced_inodes,
        root_hash,
        volume_uuid: options.volume_uuid,
        volume_name,
    };

    Ok(SealPlan {
        total_blocks,
        data_start,
        used_end,
        block_zero: nx_block,
        metadata,
        writes,
        stats,
    })
}

fn exact_container_geometry(volume_blocks: u64) -> Result<(u64, Geometry), SealError> {
    let mut blocks = volume_blocks.max(MIN_CONTAINER_BLOCKS);
    loop {
        let g = Geometry::for_blocks(blocks)?;
        let overhead = 1
            + u64::from(g.checkpoint.desc_blocks)
            + u64::from(g.checkpoint.data_blocks)
            + POOL_BITMAP_BLOCKS
            + g.pool_blocks
            + 2;
        let need = overhead
            .checked_add(volume_blocks)
            .ok_or_else(|| SealError::InvalidSpec("container block count overflow".into()))?;
        if need <= blocks {
            return Ok((blocks, g));
        }
        blocks = need;
    }
}
