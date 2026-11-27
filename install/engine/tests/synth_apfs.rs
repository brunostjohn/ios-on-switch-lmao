// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::Read;
use std::path::Path;

use sha2::{Digest, Sha256};
use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{Record, TreeShape, leaf_root};
use switch_apfs_format::container::{
    CHECKPOINT_MAP_LAST, CPM_ENTRIES_AT, CheckpointMapHeader, CheckpointMapping, NX_INCOMPAT_VERSION2, NX_MAGIC,
    NxSuperblock,
};
use switch_apfs_format::io::{BlockTarget, FileTarget};
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, ObjPhys, TYPE_BTREE, TYPE_CHECKPOINT_MAP, TYPE_NX_REAPER, TYPE_NX_SUPERBLOCK,
    TYPE_SPACEMAN, TYPE_SPACEMAN_FREE_QUEUE,
};
use switch_apfs_format::omap::{OMAP_MANUALLY_MANAGED, OmapKey, OmapVal};
use switch_apfs_format::volume::{
    APFS_FEATURE_HARDLINK_MAP_RECORDS, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_SYSTEM,
};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal as seal_cksum};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_write::place::{FreeRuns, NewContent, place};
use switch_apfs_write::records::{self, RecordInput};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::{self, VolumeIds, VolumeInput, omap_block};
use switch_apfs_write::{Attrs, CheckpointAreas, Tree, VolumeSpec, WriteOptions};
use switch_image::add_file_at;
use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromZeros, IntoBytes};

pub const SYNTH_TIME_NS: u64 = 1_795_000_000_000_000_000;
const EPHEMERAL_INFO_4096: u64 = 0x0006_0004_0001;
const NR_BHM_FLAG: u32 = 0x1;
const NR_STATE_BUFFER_SIZE: u32 = (BLOCK_SIZE - 112) as u32;

pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

pub fn sha256_file(path: &Path) -> (u64, String) {
    let mut file = File::open(path).unwrap();
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 65_536];
    let mut len = 0u64;
    loop {
        let n = file.read(&mut buf).unwrap();
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        len += n as u64;
    }
    (len, hex::encode(hasher.finalize()))
}

pub fn write_synthetic_os_container(path: &Path, system_uuid: [u8; 16], unicode: UnicodeVersion) {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let var_data = tree.add_bytes(&b"system-var-file\n"[..]);
    add_file_at(
        &mut tree,
        "/private/var/db/sample.txt",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o644),
        Some(var_data),
    )
    .unwrap();
    let var_delta = tree.add_bytes(&b"var-original-binary"[..]);
    add_file_at(
        &mut tree,
        "/private/var/db/tool",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o755),
        Some(var_delta),
    )
    .unwrap();
    let sys_bin = tree.add_bytes(&b"system-original-binary"[..]);
    add_file_at(
        &mut tree,
        "/usr/libexec/demo_bin",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o755),
        Some(sys_bin),
    )
    .unwrap();

    let total_blocks = 4096u64;
    let xid = 2u64;
    let g = Geometry::for_blocks(total_blocks).unwrap();
    let desc_base = 1u64;
    let data_base = desc_base + u64::from(g.checkpoint.desc_blocks);
    let ring_base = data_base + u64::from(g.checkpoint.data_blocks);
    let pool_base = ring_base + POOL_BITMAP_BLOCKS;
    let nx_omap = pool_base + g.pool_blocks;
    let nx_omap_tree = nx_omap + 1;

    let mut used = Bitmap::new(total_blocks);
    used.set_range(0..nx_omap_tree + 1);
    let mut free = FreeRuns::from_bitmap(&used);
    let placement = place(&[&tree], &mut free).unwrap();

    let mut mem = MemTarget::default();
    for d in &placement.new_data {
        if let NewContent::Bytes { bytes, offset } = &d.content {
            let end = (offset + d.blocks as usize * BLOCK_SIZE).min(bytes.len());
            let mut buf = bytes[*offset..end].to_vec();
            buf.resize(d.blocks as usize * BLOCK_SIZE, 0);
            mem.write_blocks(d.dst, &buf).unwrap();
        }
    }

    let spec = VolumeSpec {
        uuid: system_uuid,
        name: "System".into(),
        role: APFS_VOL_ROLE_SYSTEM,
        features: APFS_FEATURE_HARDLINK_MAP_RECORDS,
        readonly_compatible_features: 0,
        incompatible_features: 0,
        unicode,
    };
    let options = WriteOptions {
        target_blocks: total_blocks,
        areas: CheckpointAreas::Rule,
        time_ns: SYNTH_TIME_NS,
        formatted_by: "synth_os".into(),
    };
    let recs = records::build(&RecordInput {
        tree: &tree,
        extents: &placement.extents[0],
        incompatible_features: APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
        readonly_compatible_features: 0,
        unicode,
        time_ns: SYNTH_TIME_NS,
    })
    .unwrap();
    let ids = VolumeIds {
        fs_index: 0,
        superblock_oid: 0x404,
        xid,
    };
    let mut next_oid = 0x405u64;
    let mut taken = placement.taken;
    let empty_system = [0u8; BLOCK_SIZE];
    let built = volume::build(
        &VolumeInput {
            spec: &spec,
            records: &recs,
            ids: &ids,
            options: &options,
            system: &empty_system,
        },
        &mut next_oid,
        &mut free,
        &mut taken,
    )
    .unwrap();
    for (p, blk) in built.blocks {
        mem.write_blocks(p, &blk[..]).unwrap();
    }
    for run in taken {
        used.set_range(run);
    }

    let nx_recs = [omap_entry(0x404, xid, 0, built.stats.superblock)];
    let omap_tree_blk = leaf_root(&TreeShape::OMAP, nx_omap_tree, xid, &nx_recs).unwrap();
    let omap_blk = omap_block(nx_omap, nx_omap_tree, xid, OMAP_MANUALLY_MANAGED);
    mem.write_blocks(nx_omap_tree, &omap_tree_blk[..]).unwrap();
    mem.write_blocks(nx_omap, &omap_blk[..]).unwrap();

    let sp = space::build(&SpaceInput {
        geometry: &g,
        used: &used,
        pool_base,
        ring_base,
        xid,
        spaceman_oid: 0x400,
        free_queue_oids: [0x402, 0x403],
    })
    .unwrap();
    for (p, blk) in sp.blocks {
        mem.write_blocks(p, &blk[..]).unwrap();
    }

    write_checkpoint_and_superblock(
        &mut mem,
        &g,
        desc_base,
        data_base,
        nx_omap,
        next_oid,
        xid,
        sp.spaceman,
        [0x55; 16],
    );
    dump_mem_to_file(&mem, total_blocks, path);
}

pub fn omap_entry(oid: u64, xid: u64, flags: u32, paddr: u64) -> Record {
    let k = OmapKey {
        oid: U64::new(oid),
        xid: U64::new(xid),
    };
    let v = OmapVal {
        flags: U32::new(flags),
        size: U32::new(BLOCK_SIZE as u32),
        paddr: U64::new(paddr),
    };
    (k.as_bytes().to_vec(), Some(v.as_bytes().to_vec()))
}

#[allow(clippy::too_many_arguments)]
pub fn write_checkpoint_and_superblock(
    mem: &mut MemTarget,
    g: &Geometry,
    desc_base: u64,
    data_base: u64,
    nx_omap: u64,
    next_oid: u64,
    xid: u64,
    spaceman: Box<Block>,
    nx_uuid: [u8; 16],
) {
    let mut reaper_blk = Box::new([0u8; BLOCK_SIZE]);
    put(
        &mut reaper_blk[..],
        0,
        &ObjPhys::new(0x401, xid, OBJ_EPHEMERAL | TYPE_NX_REAPER, 0),
    );
    put(&mut reaper_blk[..], 32, &U64::new(1));
    put(&mut reaper_blk[..], 64, &U32::new(NR_BHM_FLAG));
    put(&mut reaper_blk[..], 108, &U32::new(NR_STATE_BUFFER_SIZE));
    seal_cksum(&mut reaper_blk[..]);

    let fq_ip = leaf_root(&TreeShape::FREE_QUEUE, 0x402, xid, &[]).unwrap();
    let fq_main = leaf_root(&TreeShape::FREE_QUEUE, 0x403, xid, &[]).unwrap();
    mem.write_blocks(data_base, &spaceman[..]).unwrap();
    mem.write_blocks(data_base + 1, &reaper_blk[..]).unwrap();
    mem.write_blocks(data_base + 2, &fq_ip[..]).unwrap();
    mem.write_blocks(data_base + 3, &fq_main[..]).unwrap();

    let mut cpm_blk = [0u8; BLOCK_SIZE];
    put(
        &mut cpm_blk,
        0,
        &CheckpointMapHeader {
            o: ObjPhys::new(desc_base, xid, OBJ_PHYSICAL | TYPE_CHECKPOINT_MAP, 0),
            flags: U32::new(CHECKPOINT_MAP_LAST),
            count: U32::new(4),
        },
    );
    let mappings = [
        (0x400u64, OBJ_EPHEMERAL | TYPE_SPACEMAN, 0u32, data_base),
        (0x401, OBJ_EPHEMERAL | TYPE_NX_REAPER, 0, data_base + 1),
        (
            0x402,
            OBJ_EPHEMERAL | TYPE_BTREE,
            TYPE_SPACEMAN_FREE_QUEUE,
            data_base + 2,
        ),
        (
            0x403,
            OBJ_EPHEMERAL | TYPE_BTREE,
            TYPE_SPACEMAN_FREE_QUEUE,
            data_base + 3,
        ),
    ];
    for (k, (oid, ty, sub, paddr)) in mappings.into_iter().enumerate() {
        let m = CheckpointMapping {
            type_and_flags: U32::new(ty),
            subtype: U32::new(sub),
            size: U32::new(BLOCK_SIZE as u32),
            pad: U32::ZERO,
            fs_oid: U64::ZERO,
            oid: U64::new(oid),
            paddr: U64::new(paddr),
        };
        put(&mut cpm_blk, CPM_ENTRIES_AT + k * size_of::<CheckpointMapping>(), &m);
    }
    seal_cksum(&mut cpm_blk);
    mem.write_blocks(desc_base, &cpm_blk).unwrap();

    let mut nx = NxSuperblock::new_zeroed();
    nx.o = ObjPhys::new(1, xid, OBJ_EPHEMERAL | TYPE_NX_SUPERBLOCK, 0);
    nx.magic = U32::new(NX_MAGIC);
    nx.block_size = U32::new(BLOCK_SIZE as u32);
    nx.block_count = U64::new(g.blocks);
    nx.incompatible_features = U64::new(NX_INCOMPAT_VERSION2);
    nx.uuid = nx_uuid;
    nx.next_oid = U64::new(next_oid);
    nx.next_xid = U64::new(xid + 1);
    nx.xp_desc_blocks = U32::new(g.checkpoint.desc_blocks);
    nx.xp_data_blocks = U32::new(g.checkpoint.data_blocks);
    nx.xp_desc_base = U64::new(desc_base);
    nx.xp_data_base = U64::new(data_base);
    nx.xp_desc_next = U32::new(2);
    nx.xp_data_next = U32::new(4);
    nx.xp_desc_len = U32::new(2);
    nx.xp_data_len = U32::new(4);
    nx.spaceman_oid = U64::new(0x400);
    nx.omap_oid = U64::new(nx_omap);
    nx.reaper_oid = U64::new(0x401);
    nx.max_file_systems = U32::new(g.max_file_systems);
    nx.fs_oid[0] = U64::new(0x404);
    nx.ephemeral_info[0] = U64::new(EPHEMERAL_INFO_4096);
    let mut blk = [0u8; BLOCK_SIZE];
    put(&mut blk, 0, &nx);
    seal_cksum(&mut blk);
    mem.write_blocks(desc_base + 1, &blk).unwrap();
    mem.write_blocks(0, &blk).unwrap();
}

pub fn dump_mem_to_file(mem: &MemTarget, total_blocks: u64, path: &Path) {
    let mut target = FileTarget::create(path, total_blocks * BLOCK_SIZE as u64).unwrap();
    for (&paddr, blk) in &mem.blocks {
        target.write_blocks(paddr, &blk[..]).unwrap();
    }
    target.flush().unwrap();
}
