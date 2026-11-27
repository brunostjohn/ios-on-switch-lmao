// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io::{Cursor, Write};
use std::path::Path;
use std::sync::Arc;

use flate2::Compression;
use flate2::write::ZlibEncoder;
use plist::Value as PlistValue;
use sha2::{Digest, Sha256};
use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{TreeShape, leaf_root};
use switch_apfs_format::container::{
    CHECKPOINT_MAP_LAST, CPM_ENTRIES_AT, CheckpointMapHeader, CheckpointMapping, NX_INCOMPAT_VERSION2, NX_MAGIC,
    NxSuperblock,
};
use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, UF_COMPRESSED, XATTR_DATA_EMBEDDED};
use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, ObjPhys, TYPE_BTREE, TYPE_CHECKPOINT_MAP, TYPE_NX_REAPER, TYPE_NX_SUPERBLOCK,
    TYPE_SPACEMAN, TYPE_SPACEMAN_FREE_QUEUE,
};
use switch_apfs_format::omap::{OMAP_MANUALLY_MANAGED, OmapKey, OmapVal};
use switch_apfs_format::volume::{
    APFS_FEATURE_HARDLINK_MAP_RECORDS, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_SYSTEM,
};
use switch_apfs_format::{BLOCK_SIZE, put, seal as seal_cksum};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::place::{FreeRuns, NewContent, place};
use switch_apfs_write::records::{self, RecordInput};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::{self, VolumeIds, VolumeInput, omap_block};
use switch_apfs_write::{
    Attrs, CheckpointAreas, Content, Tree, VolumeSpec, WriteOptions, Xattr, XattrValue, copy_subtree,
};
use switch_image::add_file_at;
use tempfile::TempDir;
use zerocopy::little_endian::{U32, U64};
use zerocopy::{FromZeros, IntoBytes};

use crate::{Error, GpuStage, REQUIRED_PLUGIN_PATH, stage_gpu};

fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

fn read_bytes(tree: &Tree, path: &str) -> Vec<u8> {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    match tree.inode(inode).data {
        None => Vec::new(),
        Some(s) => match &tree.stream(s).content {
            Content::Bytes(b) => b.to_vec(),
            other => panic!("{path}: expected Content::Bytes, got {other:?}"),
        },
    }
}

fn make_decmpfs_type3(plain: &[u8]) -> Vec<u8> {
    let mut enc = ZlibEncoder::new(Vec::new(), Compression::default());
    enc.write_all(plain).unwrap();
    let compressed = enc.finish().unwrap();
    let mut out = Vec::with_capacity(16 + compressed.len());
    out.extend_from_slice(b"fpmc");
    out.extend_from_slice(&3u32.to_le_bytes());
    out.extend_from_slice(&(plain.len() as u64).to_le_bytes());
    out.extend_from_slice(&compressed);
    out
}

#[test]
fn stage_gpu_merges_source_backed_and_decmpfs_compressed_plists() {
    let tmp = TempDir::new().unwrap();
    let os_path = tmp.path().join("source.apfs");
    let sys_uuid = [0x11u8; 16];
    let plain_xml = b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
        <plist version=\"1.0\"><dict><key>FromSource</key><string>plain</string></dict></plist>\n";
    let decmpfs_xml = b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
        <plist version=\"1.0\"><dict><key>FromCompressed</key><integer>42</integer></dict></plist>\n";

    write_source_container_with_plists(&os_path, sys_uuid, plain_xml, decmpfs_xml);

    let src = FileSource::open(&os_path, 0).unwrap();
    let container = Container::open(&src).unwrap();
    let vol = container.volume(sys_uuid).unwrap();
    let fs = Fs::open(&src, vol).unwrap();

    let mut data_tree = Tree::new(Attrs::new(0, 0, 0o755));
    let root = data_tree.root();
    copy_subtree(&fs, "/private/var", &mut data_tree, root).unwrap();

    let plain_ino = data_tree
        .lookup("mobile/Library/Preferences/com.apple.backboardd.plist")
        .unwrap();
    let plain_stream = data_tree.inode(plain_ino).data.unwrap();
    assert!(matches!(data_tree.stream(plain_stream).content, Content::Source(_)));

    let cmp_ino = data_tree
        .lookup("root/Library/Preferences/com.apple.backboardd.plist")
        .unwrap();
    assert_ne!(data_tree.inode(cmp_ino).attrs.bsd_flags & UF_COMPRESSED, 0);

    let plugin = b"plugin-for-source-test";
    let manifest = format!(
        "# gpu stage 2 tree\n\
         file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n\
         pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableDeepFramebuffer\tfalse\n\
         pref\troot/Library/Preferences/com.apple.backboardd.plist\t0\tCAEnableDeepFramebuffer\tfalse\n",
        sha256_hex(plugin),
        plugin.len(),
    );
    let mut mem = BTreeMap::new();
    mem.insert(REQUIRED_PLUGIN_PATH.to_owned(), Arc::from(plugin.as_slice()));
    let stage = GpuStage::parse_manifest_memory(&manifest, &mem).unwrap();

    assert!(matches!(
        stage_gpu(&mut data_tree, None, &stage),
        Err(Error::MissingSourceFs(_))
    ));
    assert!(data_tree.lookup(REQUIRED_PLUGIN_PATH).is_none());

    let report = stage_gpu(&mut data_tree, Some(&fs), &stage).unwrap();
    assert_eq!(report.files, 1);
    assert_eq!(report.prefs, 2);

    let merged_plain = read_bytes(&data_tree, "mobile/Library/Preferences/com.apple.backboardd.plist");
    assert!(merged_plain.starts_with(b"bplist00"));
    let d1 = PlistValue::from_reader(Cursor::new(&merged_plain))
        .unwrap()
        .into_dictionary()
        .unwrap();
    assert_eq!(d1.get("FromSource").and_then(PlistValue::as_string), Some("plain"));
    assert_eq!(
        d1.get("CAEnableDeepFramebuffer").and_then(PlistValue::as_boolean),
        Some(false)
    );

    let merged_cmp = read_bytes(&data_tree, "root/Library/Preferences/com.apple.backboardd.plist");
    assert!(merged_cmp.starts_with(b"bplist00"));
    assert_eq!(data_tree.inode(cmp_ino).attrs.bsd_flags & UF_COMPRESSED, 0);
    let d2 = PlistValue::from_reader(Cursor::new(&merged_cmp))
        .unwrap()
        .into_dictionary()
        .unwrap();
    assert_eq!(
        d2.get("FromCompressed").and_then(PlistValue::as_signed_integer),
        Some(42)
    );
    assert_eq!(
        d2.get("CAEnableDeepFramebuffer").and_then(PlistValue::as_boolean),
        Some(false)
    );
}

fn write_source_container_with_plists(path: &Path, system_uuid: [u8; 16], plain_xml: &[u8], decmpfs_xml: &[u8]) {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let plain_stream = tree.add_bytes(plain_xml.to_vec());
    add_file_at(
        &mut tree,
        "/private/var/mobile/Library/Preferences/com.apple.backboardd.plist",
        Attrs::new(501, 501, 0o755),
        Attrs::new(501, 501, 0o644),
        Some(plain_stream),
    )
    .unwrap();
    let cmp_inode = add_file_at(
        &mut tree,
        "/private/var/root/Library/Preferences/com.apple.backboardd.plist",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o644),
        None,
    )
    .unwrap();
    tree.inode_mut(cmp_inode).attrs.bsd_flags |= UF_COMPRESSED;
    tree.put_xattr(
        cmp_inode,
        Xattr {
            name: DECMPFS_XATTR_NAME.into(),
            flags: XATTR_DATA_EMBEDDED,
            value: XattrValue::Embedded(make_decmpfs_type3(decmpfs_xml)),
        },
    );

    let total_blocks = 4096u64;
    let xid = 2u64;
    let time_ns = 1_795_000_000_000_000_000u64;
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
        unicode: UnicodeVersion::IOS_27,
    };
    let options = WriteOptions {
        target_blocks: total_blocks,
        areas: CheckpointAreas::Rule,
        time_ns,
        formatted_by: "gpu_test".into(),
    };
    let recs = records::build(&RecordInput {
        tree: &tree,
        extents: &placement.extents[0],
        incompatible_features: APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
        readonly_compatible_features: 0,
        unicode: UnicodeVersion::IOS_27,
        time_ns,
    })
    .unwrap();
    let ids = VolumeIds {
        fs_index: 0,
        superblock_oid: 0x404,
        xid,
    };
    let mut next_oid = 0x405u64;
    let mut taken = placement.taken;
    let empty_sys = [0u8; BLOCK_SIZE];
    let built = volume::build(
        &VolumeInput {
            spec: &spec,
            records: &recs,
            ids: &ids,
            options: &options,
            system: &empty_sys,
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

    let k = OmapKey {
        oid: U64::new(0x404),
        xid: U64::new(xid),
    };
    let v = OmapVal {
        flags: U32::ZERO,
        size: U32::new(BLOCK_SIZE as u32),
        paddr: U64::new(built.stats.superblock),
    };
    let nx_recs = [(k.as_bytes().to_vec(), Some(v.as_bytes().to_vec()))];
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

    let mut reaper_blk = [0u8; BLOCK_SIZE];
    put(
        &mut reaper_blk,
        0,
        &ObjPhys::new(0x401, xid, OBJ_EPHEMERAL | TYPE_NX_REAPER, 0),
    );
    put(&mut reaper_blk, 32, &U64::new(1));
    put(&mut reaper_blk, 64, &U32::new(1));
    put(&mut reaper_blk, 108, &U32::new((BLOCK_SIZE - 112) as u32));
    seal_cksum(&mut reaper_blk);

    let fq_ip = leaf_root(&TreeShape::FREE_QUEUE, 0x402, xid, &[]).unwrap();
    let fq_main = leaf_root(&TreeShape::FREE_QUEUE, 0x403, xid, &[]).unwrap();
    mem.write_blocks(data_base, &sp.spaceman[..]).unwrap();
    mem.write_blocks(data_base + 1, &reaper_blk).unwrap();
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
    for (idx, (oid, ty, sub, paddr)) in mappings.into_iter().enumerate() {
        let m = CheckpointMapping {
            type_and_flags: U32::new(ty),
            subtype: U32::new(sub),
            size: U32::new(BLOCK_SIZE as u32),
            pad: U32::ZERO,
            fs_oid: U64::ZERO,
            oid: U64::new(oid),
            paddr: U64::new(paddr),
        };
        put(&mut cpm_blk, CPM_ENTRIES_AT + idx * size_of::<CheckpointMapping>(), &m);
    }
    seal_cksum(&mut cpm_blk);
    mem.write_blocks(desc_base, &cpm_blk).unwrap();

    let mut nx = NxSuperblock::new_zeroed();
    nx.o = ObjPhys::new(1, xid, OBJ_EPHEMERAL | TYPE_NX_SUPERBLOCK, 0);
    nx.magic = U32::new(NX_MAGIC);
    nx.block_size = U32::new(BLOCK_SIZE as u32);
    nx.block_count = U64::new(g.blocks);
    nx.incompatible_features = U64::new(NX_INCOMPAT_VERSION2);
    nx.uuid = [0x44; 16];
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
    nx.ephemeral_info[0] = U64::new(0x0006_0004_0001);
    let mut blk = [0u8; BLOCK_SIZE];
    put(&mut blk, 0, &nx);
    seal_cksum(&mut blk);
    mem.write_blocks(desc_base + 1, &blk).unwrap();
    mem.write_blocks(0, &blk).unwrap();

    let mut target = FileTarget::create(path, total_blocks * BLOCK_SIZE as u64).unwrap();
    for (&paddr, blk) in &mem.blocks {
        target.write_blocks(paddr, &blk[..]).unwrap();
    }
    target.flush().unwrap();
}
