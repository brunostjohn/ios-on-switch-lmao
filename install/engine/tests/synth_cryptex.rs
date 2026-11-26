// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;
use std::path::Path;

use sha2::{Digest, Sha256};
use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{Layout, Record, TreeShape, leaf_root};
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_INODE, DT_DIR, DT_REG, DrecVal, Dstream, DstreamIdVal,
    INO_EXT_TYPE_DSTREAM, INO_EXT_TYPE_NAME, INODE_NO_RSRC_FORK, InodeVal, JKey, PRIV_DIR_INO_NUM, PRIV_DIR_NAME,
    ROOT_DIR_INO_NUM, ROOT_DIR_NAME, ROOT_DIR_PARENT, S_IFDIR, S_IFREG, XF_DO_NOT_COPY, XF_SYSTEM_FIELD,
};
use switch_apfs_format::fs_records::{
    APFS_FILE_INFO_DATA_HASH, APFS_HASH_SHA256, APFS_HASH_SHA256_SIZE, FextKey, FextVal, IntegrityMetaPhys,
    TYPE_INTEGRITY_META,
};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_format::object::{OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BTREE, TYPE_FS};
use switch_apfs_format::omap::{OMAP_MANUALLY_MANAGED, OMAP_VAL_NOHEADER};
use switch_apfs_format::volume::{
    APFS_FEATURE_HARDLINK_MAP_RECORDS, APFS_FS_UNENCRYPTED, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
    APFS_INCOMPAT_SEALED_VOLUME, APFS_MAGIC, ApfsSuperblock,
};
use switch_apfs_format::{BLOCK_SIZE, Block, put, seal as seal_cksum};
use switch_apfs_names::{UnicodeVersion, name_len_and_hash};
use switch_apfs_read::{Container, Fs, Step, XField, stream};
use switch_apfs_seal::file_info::{encode_file_info, hash_reader};
use switch_apfs_write::extents::{self, Owned};
use switch_apfs_write::inode::xfields;
use switch_apfs_write::keys::fs_key_order;
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::omap_block;
use zerocopy::little_endian::{I32, U16, U32, U64};
use zerocopy::{FromZeros, IntoBytes};

use crate::synth_apfs::{SYNTH_TIME_NS, dump_mem_to_file, omap_entry, write_checkpoint_and_superblock};

pub fn write_synthetic_sealed_cryptex(path: &Path, vol_uuid: [u8; 16], vol_name: &str, files: &[(&str, &[u8])]) {
    let total_blocks = 4096u64;
    let xid = 2u64;
    let g = Geometry::for_blocks(total_blocks).unwrap();
    let mut mem = MemTarget::default();
    let mut fs_recs: Vec<Record> = Vec::new();
    let mut fext_recs: Vec<Record> = Vec::new();
    let mut owned: Vec<Owned> = Vec::new();
    let mut data_block = 500u64;

    push_drec(&mut fs_recs, ROOT_DIR_PARENT, ROOT_DIR_NAME, ROOT_DIR_INO_NUM, DT_DIR);
    push_inode(
        &mut fs_recs,
        ROOT_DIR_INO_NUM,
        ROOT_DIR_PARENT,
        ROOT_DIR_NAME,
        S_IFDIR | 0o755,
        files.len() as i32,
        None,
    );
    push_drec(&mut fs_recs, ROOT_DIR_PARENT, PRIV_DIR_NAME, PRIV_DIR_INO_NUM, DT_DIR);
    push_inode(
        &mut fs_recs,
        PRIV_DIR_INO_NUM,
        ROOT_DIR_PARENT,
        PRIV_DIR_NAME,
        S_IFDIR | 0o644,
        0,
        None,
    );

    for (oid, (name, data)) in (16u64..).zip(files.iter()) {
        let blocks = data.len().div_ceil(BLOCK_SIZE).max(1) as u64;
        let paddr = data_block;
        data_block += blocks;
        let mut buf = vec![0u8; blocks as usize * BLOCK_SIZE];
        buf[..data.len()].copy_from_slice(data);
        mem.write_blocks(paddr, &buf).unwrap();

        let alloced = blocks * BLOCK_SIZE as u64;
        let ds_val = DstreamIdVal { refcnt: U32::new(1) };
        fs_recs.push((
            JKey::new(oid, APFS_TYPE_DSTREAM_ID).as_bytes().to_vec(),
            Some(ds_val.as_bytes().to_vec()),
        ));
        for chunk in &hash_reader(&mut Cursor::new(data)).unwrap().chunks {
            fs_recs.push(encode_file_info(oid, chunk));
        }
        let fk = FextKey {
            private_id: U64::new(oid),
            logical_addr: U64::new(0),
        };
        let fv = FextVal {
            len_and_flags: U64::new(alloced),
            phys_block_num: U64::new(paddr),
        };
        fext_recs.push((fk.as_bytes().to_vec(), Some(fv.as_bytes().to_vec())));
        owned.push(Owned {
            paddr,
            blocks,
            owner: oid,
        });

        let dstream = Dstream {
            size: U64::new(data.len() as u64),
            alloced_size: U64::new(alloced),
            ..Default::default()
        };
        push_drec(&mut fs_recs, ROOT_DIR_INO_NUM, name, oid, DT_REG);
        push_inode(
            &mut fs_recs,
            oid,
            ROOT_DIR_INO_NUM,
            name,
            S_IFREG | 0o755,
            1,
            Some(dstream),
        );
    }

    fs_recs.sort_by(|a, b| fs_key_order(&a.0, &b.0));
    let fs_layout = Layout::new(&TreeShape::SEALED_FS_TREE, &fs_recs).unwrap();
    let mut oid_cur = 0x406u64;
    let fs_built = fs_layout.emit(xid, &mut || {
        let id = oid_cur;
        oid_cur += 1;
        id
    });
    let root_hash = fs_built.root_hash.unwrap();

    let desc_base = 1u64;
    let data_base = desc_base + u64::from(g.checkpoint.desc_blocks);
    let ring_base = data_base + u64::from(g.checkpoint.data_blocks);
    let pool_base = ring_base + POOL_BITMAP_BLOCKS;
    let nx_omap = pool_base + g.pool_blocks;
    let [
        nx_omap_tree,
        vol_sb,
        vol_omap,
        vol_omap_tree,
        im_p,
        snap_p,
        fext_p,
        er_p,
        fs_base,
    ] = std::array::from_fn(|i| nx_omap + 1 + i as u64);

    fext_recs.sort_by_key(|(k, _)| {
        let fk: FextKey = switch_apfs_format::get(k, 0).unwrap();
        (fk.private_id.get(), fk.logical_addr.get())
    });
    let fext_blk = leaf_root(&TreeShape::FEXT, fext_p, xid, &fext_recs).unwrap();
    mem.write_blocks(fext_p, &fext_blk[..]).unwrap();

    let mut vol_omap_recs = vec![omap_entry(0x405, xid, 0, im_p)];
    for (i, n) in fs_built.nodes.into_iter().enumerate() {
        let p = fs_base + i as u64;
        mem.write_blocks(p, &n.block[..]).unwrap();
        vol_omap_recs.push(omap_entry(n.id, xid, OMAP_VAL_NOHEADER, p));
    }
    let fs_end = fs_base + vol_omap_recs.len() as u64 - 1;
    assert!(fs_end <= 500);

    let er_built = extents::build(&owned);
    let nx_recs = [omap_entry(0x404, xid, 0, vol_sb)];
    for (p, blk) in [
        (
            vol_omap_tree,
            leaf_root(&TreeShape::OMAP, vol_omap_tree, xid, &vol_omap_recs).unwrap(),
        ),
        (vol_omap, omap_block(vol_omap, vol_omap_tree, xid, 0)),
        (im_p, integrity_block(0x405, xid, &root_hash)),
        (snap_p, leaf_root(&TreeShape::SNAP_META, snap_p, xid, &[]).unwrap()),
        (
            er_p,
            leaf_root(&TreeShape::EXTENT_REF, er_p, xid, &er_built.records).unwrap(),
        ),
        (
            nx_omap_tree,
            leaf_root(&TreeShape::OMAP, nx_omap_tree, xid, &nx_recs).unwrap(),
        ),
        (nx_omap, omap_block(nx_omap, nx_omap_tree, xid, OMAP_MANUALLY_MANAGED)),
    ] {
        mem.write_blocks(p, &blk[..]).unwrap();
    }

    let mut sb = ApfsSuperblock::new_zeroed();
    sb.o = ObjPhys::new(0x404, xid, OBJ_VIRTUAL | TYPE_FS, 0);
    sb.magic = U32::new(APFS_MAGIC);
    sb.features = U64::new(APFS_FEATURE_HARDLINK_MAP_RECORDS);
    sb.incompatible_features = U64::new(APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | APFS_INCOMPAT_SEALED_VOLUME);
    sb.fs_alloc_count = U64::new((fs_end - vol_sb - 1) + er_built.blocks);
    sb.root_tree_type = U32::new(OBJ_VIRTUAL | TYPE_BTREE);
    sb.extentref_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.snap_meta_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.fext_tree_type = U32::new(OBJ_PHYSICAL | TYPE_BTREE);
    sb.omap_oid = U64::new(vol_omap);
    sb.root_tree_oid = U64::new(0x406);
    sb.extentref_tree_oid = U64::new(er_p);
    sb.snap_meta_tree_oid = U64::new(snap_p);
    sb.next_obj_id = U64::new(100);
    sb.num_files = U64::new(files.len() as u64);
    sb.num_directories = U64::new(1);
    sb.total_blocks_alloced = U64::new(er_built.blocks);
    sb.vol_uuid = vol_uuid;
    sb.fs_flags = U64::new(APFS_FS_UNENCRYPTED);
    let n = vol_name.len().min(sb.volname.len() - 1);
    sb.volname[..n].copy_from_slice(&vol_name.as_bytes()[..n]);
    sb.integrity_meta_oid = U64::new(0x405);
    sb.fext_tree_oid = U64::new(fext_p);
    let mut blk = [0u8; BLOCK_SIZE];
    put(&mut blk, 0, &sb);
    seal_cksum(&mut blk);
    mem.write_blocks(vol_sb, &blk).unwrap();

    let mut used = Bitmap::new(total_blocks);
    used.set_range(0..fs_end);
    if data_block > 500 {
        used.set_range(500..data_block);
    }
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
    for (p, sblk) in sp.blocks {
        mem.write_blocks(p, &sblk[..]).unwrap();
    }

    write_checkpoint_and_superblock(
        &mut mem,
        &g,
        desc_base,
        data_base,
        nx_omap,
        oid_cur,
        xid,
        sp.spaceman,
        [0x77; 16],
    );
    dump_mem_to_file(&mem, total_blocks, path);
}

pub fn verify_sealed_cryptex_bytes(bytes: &[u8]) -> Vec<(String, Vec<u8>)> {
    assert!(bytes.len().is_multiple_of(BLOCK_SIZE));
    struct SliceSource<'a>(&'a [u8]);
    impl BlockSource for SliceSource<'_> {
        fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> std::io::Result<()> {
            let start = paddr as usize * BLOCK_SIZE;
            out.copy_from_slice(&self.0[start..start + out.len()]);
            Ok(())
        }
    }
    let src = SliceSource(bytes);
    let container = Container::open(&src).unwrap();
    let audit = switch_apfs_read::audit(&container, &src).unwrap();
    assert!(audit.ok(), "sealed cryptex audit failed:\n{audit}");
    assert_eq!(container.volumes.len(), 1);
    let fs = Fs::open(&src, &container.volumes[0]).unwrap();
    assert!(fs.seal.is_some(), "cryptex volume must be sealed");

    let mut files = Vec::new();
    fs.walk("/", &mut |entry| {
        if entry.inode.is_regular() {
            let mut data = Vec::new();
            if let Some(s) = &entry.data {
                stream::read(&src, s, &mut |chunk| data.extend_from_slice(chunk))?;
                assert!(!s.file_info.is_empty(), "{}: sealed file lacks file_info", entry.path);
                for info in &s.file_info {
                    assert_eq!(info.info_type, APFS_FILE_INFO_DATA_HASH);
                    let start = info.offset as usize;
                    let end = start + usize::from(info.hashed_len) * BLOCK_SIZE;
                    let mut h = Sha256::new();
                    let have = &data[start..end.min(data.len())];
                    h.update(have);
                    h.update(vec![0u8; end - start - have.len()]);
                    assert_eq!(h.finalize().as_slice(), info.hash.as_slice());
                }
            }
            files.push((entry.path.clone(), data));
        }
        Ok(Step::Into)
    })
    .unwrap();
    files
}

fn integrity_block(oid: u64, xid: u64, root_hash: &[u8; APFS_HASH_SHA256_SIZE]) -> Box<Block> {
    let meta = IntegrityMetaPhys {
        o: ObjPhys::new(oid, xid, OBJ_VIRTUAL | TYPE_INTEGRITY_META, 0),
        version: U32::new(2),
        flags: U32::ZERO,
        hash_type: U32::new(APFS_HASH_SHA256),
        root_hash_offset: U32::new(size_of::<IntegrityMetaPhys>() as u32),
        broken_xid: U64::ZERO,
        reserved: [U64::ZERO; 9],
    };
    let mut blk = Box::new([0u8; BLOCK_SIZE]);
    put(&mut blk[..], 0, &meta);
    let off = size_of::<IntegrityMetaPhys>();
    blk[off..off + APFS_HASH_SHA256_SIZE].copy_from_slice(root_hash);
    seal_cksum(&mut blk[..]);
    blk
}

fn push_drec(fs: &mut Vec<Record>, parent: u64, name: &str, oid: u64, dtype: u16) {
    let mut k = JKey::new(parent, APFS_TYPE_DIR_REC).as_bytes().to_vec();
    k.extend_from_slice(&name_len_and_hash(name, UnicodeVersion::IOS_27).unwrap().to_le_bytes());
    k.extend_from_slice(name.as_bytes());
    k.push(0);
    let v = DrecVal {
        file_id: U64::new(oid),
        date_added: U64::new(SYNTH_TIME_NS),
        flags: U16::new(dtype),
    };
    fs.push((k, Some(v.as_bytes().to_vec())));
}

fn push_inode(
    fs: &mut Vec<Record>,
    oid: u64,
    parent: u64,
    name: &str,
    mode: u16,
    nlink: i32,
    dstream: Option<Dstream>,
) {
    let mut val = InodeVal::new_zeroed();
    val.parent_id = U64::new(parent);
    val.private_id = U64::new(oid);
    val.create_time = U64::new(SYNTH_TIME_NS);
    val.mod_time = U64::new(SYNTH_TIME_NS);
    val.change_time = U64::new(SYNTH_TIME_NS);
    val.access_time = U64::new(SYNTH_TIME_NS);
    val.internal_flags = U64::new(INODE_NO_RSRC_FORK);
    val.nchildren_or_nlink = I32::new(nlink);
    val.mode = U16::new(mode);
    let mut name_nul = name.as_bytes().to_vec();
    name_nul.push(0);
    let mut xf = vec![XField {
        kind: INO_EXT_TYPE_NAME,
        flags: XF_DO_NOT_COPY,
        data: name_nul,
    }];
    if let Some(ds) = dstream {
        xf.push(XField {
            kind: INO_EXT_TYPE_DSTREAM,
            flags: XF_SYSTEM_FIELD,
            data: ds.as_bytes().to_vec(),
        });
    }
    let mut v = val.as_bytes().to_vec();
    v.extend_from_slice(&xfields(&xf));
    fs.push((JKey::new(oid, APFS_TYPE_INODE).as_bytes().to_vec(), Some(v)));
}
