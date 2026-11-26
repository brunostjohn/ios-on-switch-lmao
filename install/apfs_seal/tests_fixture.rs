// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;

use switch_apfs_alloc::geometry::POOL_BITMAP_BLOCKS;
use switch_apfs_alloc::space::{self, SpaceInput};
use switch_apfs_alloc::{Bitmap, Geometry};
use switch_apfs_btree::{Layout, Record, TreeShape, leaf_root};
use switch_apfs_format::container::{NX_INCOMPAT_VERSION2, NX_MAGIC, NxSuperblock};
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_INODE, APFS_TYPE_XATTR, DT_DIR, DT_LNK, DT_REG, DrecVal,
    Dstream, DstreamIdVal, INO_EXT_TYPE_DSTREAM, INO_EXT_TYPE_NAME, INODE_NO_RSRC_FORK, InodeVal, JKey,
    PRIV_DIR_INO_NUM, PRIV_DIR_NAME, ROOT_DIR_INO_NUM, ROOT_DIR_NAME, ROOT_DIR_PARENT, S_IFDIR, S_IFREG,
    XF_DO_NOT_COPY, XF_SYSTEM_FIELD,
};
use switch_apfs_format::fs_records::{
    APFS_TYPE_SIBLING_LINK, APFS_TYPE_SIBLING_MAP, DECMPFS_MAGIC, DECMPFS_XATTR_NAME, DREC_EXT_TYPE_SIBLING_ID,
    DecmpfsHeader, FextKey, RESOURCE_FORK_XATTR_NAME, S_IFLNK, SYMLINK_XATTR_NAME, SiblingKey, SiblingVal,
    UF_COMPRESSED, XATTR_DATA_EMBEDDED, XATTR_DATA_STREAM, XATTR_FILE_SYSTEM_OWNED, XattrDstream, XattrVal,
};
use switch_apfs_format::io::BlockTarget;
use switch_apfs_format::object::{
    OBJ_EPHEMERAL, OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BTREE, TYPE_FS, TYPE_NX_SUPERBLOCK,
};
use switch_apfs_format::omap::{OMAP_MANUALLY_MANAGED, OMAP_VAL_NOHEADER};
use switch_apfs_format::volume::{
    APFS_FEATURE_HARDLINK_MAP_RECORDS, APFS_FS_UNENCRYPTED, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
    APFS_INCOMPAT_SEALED_VOLUME, APFS_MAGIC, ApfsSuperblock,
};
use switch_apfs_format::{BLOCK_SIZE, put, seal as seal_cksum};
use switch_apfs_names::{UnicodeVersion, name_len_and_hash};
use switch_apfs_read::XField;
use switch_apfs_write::extents::{self, Owned};
use switch_apfs_write::inode::{
    INODE_HAS_RSRC_FORK, INODE_HAS_SECURITY_EA, INODE_HAS_UNCOMPRESSED_SIZE, SECURITY_XATTR_NAME, xfields,
};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::omap_block;
use zerocopy::little_endian::{I32, U16, U32, U64};
use zerocopy::{FromZeros, IntoBytes};

use crate::blocks::{checkpoint_blocks, emit_physical, integrity_block, omap_entry};
use crate::file_info::{encode_file_info, hash_reader};
use crate::place::encode_fext;
use crate::records::sort_fs_records;

const TEST_TIME_NS: u64 = 1_700_000_000_000_000_000;
const DECMPFS_STORED_MARKER: u8 = 0xCC;
const EPHEMERAL_INFO_4096: u64 = 0x0006_0004_0001;

#[derive(Default)]
pub(crate) struct Fixture {
    pub(crate) mem: MemTarget,
    pub(crate) fs: Vec<Record>,
    pub(crate) fext: Vec<Record>,
    pub(crate) owned: Vec<Owned>,
    pub(crate) data_block: u64,
    pub(crate) next_obj_id: u64,
    pub(crate) num_files: u64,
    pub(crate) num_dirs: u64,
    pub(crate) num_symlinks: u64,
}

#[derive(Default)]
pub(crate) struct InodeMeta {
    pub(crate) private_id: Option<u64>,
    pub(crate) nlink: i32,
    pub(crate) bsd_flags: u32,
    pub(crate) iflags: u64,
    pub(crate) uncompressed_size: u64,
    pub(crate) dstream: Option<Dstream>,
}

impl Fixture {
    pub(crate) fn empty_root(root_children: i32) -> Self {
        let mut b = Self {
            data_block: 500,
            next_obj_id: 100,
            ..Default::default()
        };
        for (oid, name, mode, nlink) in [
            (ROOT_DIR_INO_NUM, ROOT_DIR_NAME, 0o755, root_children),
            (PRIV_DIR_INO_NUM, PRIV_DIR_NAME, 0o644, 0),
        ] {
            let m = InodeMeta {
                nlink,
                iflags: INODE_NO_RSRC_FORK,
                ..Default::default()
            };
            b.add_entry(oid, ROOT_DIR_PARENT, name, DT_DIR, S_IFDIR | mode, m);
        }
        b
    }

    pub(crate) fn write(&mut self, paddr: u64, data: &[u8]) {
        self.mem.write_blocks(paddr, data).unwrap();
    }

    pub(crate) fn push_dstream_id(&mut self, sid: u64, refcnt: u32) {
        let val = DstreamIdVal {
            refcnt: U32::new(refcnt),
        };
        self.fs.push((
            JKey::new(sid, APFS_TYPE_DSTREAM_ID).as_bytes().to_vec(),
            Some(val.as_bytes().to_vec()),
        ));
    }

    pub(crate) fn push_fext(&mut self, sid: u64, logical: u64, len: u64, flags: u8, paddr: u64) {
        self.fext.push(encode_fext(sid, logical, len, flags, paddr));
    }

    pub(crate) fn write_blocks_data(&mut self, data: &[u8]) -> (u64, u64) {
        let blocks = data.len().div_ceil(BLOCK_SIZE).max(1) as u64;
        let paddr = self.data_block;
        let mut buf = vec![0u8; blocks as usize * BLOCK_SIZE];
        buf[..data.len()].copy_from_slice(data);
        self.write(paddr, &buf);
        self.data_block += blocks;
        (paddr, blocks)
    }

    pub(crate) fn add_stream_refcnt(&mut self, sid: u64, owner: u64, data: &[u8], refcnt: Option<u32>) -> Dstream {
        let (paddr, blocks) = self.write_blocks_data(data);
        let alloced = blocks * BLOCK_SIZE as u64;
        if let Some(rc) = refcnt {
            self.push_dstream_id(sid, rc);
        }
        for c in &hash_reader(&mut Cursor::new(data)).unwrap().chunks {
            self.fs.push(encode_file_info(sid, c));
        }
        self.push_fext(sid, 0, alloced, 0, paddr);
        self.owned.push(Owned { paddr, blocks, owner });
        Dstream {
            size: U64::new(data.len() as u64),
            alloced_size: U64::new(alloced),
            ..Default::default()
        }
    }

    pub(crate) fn add_entry(&mut self, oid: u64, parent: u64, name: &str, dtype: u16, mode: u16, m: InodeMeta) {
        push_drec(&mut self.fs, parent, name, oid, dtype, None);
        push_inode(&mut self.fs, oid, parent, name, mode, &m);
    }

    pub(crate) fn add_xattr(&mut self, oid: u64, name: &str, flags: u16, payload: &[u8]) {
        let mut k = JKey::new(oid, APFS_TYPE_XATTR).as_bytes().to_vec();
        k.extend_from_slice(&(name.len() as u16 + 1).to_le_bytes());
        k.extend_from_slice(name.as_bytes());
        k.push(0);
        let head = XattrVal {
            flags: U16::new(flags),
            xdata_len: U16::new(payload.len() as u16),
        };
        let mut v = head.as_bytes().to_vec();
        v.extend_from_slice(payload);
        self.fs.push((k, Some(v)));
    }

    pub(crate) fn finish(mut self) -> MemTarget {
        let (total_blocks, xid) = (4096u64, 2u64);
        let g = Geometry::for_blocks(total_blocks).unwrap();

        sort_fs_records(&mut self.fs);
        let fs_layout = Layout::new(&TreeShape::SEALED_FS_TREE, &self.fs).unwrap();
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

        self.fext.sort_by_key(|(k, _)| {
            let fk: FextKey = switch_apfs_format::get(k, 0).unwrap();
            (fk.private_id.get(), fk.logical_addr.get())
        });
        let fext_layout = Layout::new(&TreeShape::FEXT, &self.fext).unwrap();
        let fext_n = fext_layout.node_count() as u64;
        let (er_p, fs_base) = (er_p + fext_n.saturating_sub(1), fs_base + fext_n.saturating_sub(1));
        let fext_addrs: Vec<u64> = (fext_p..fext_p + fext_n).collect();
        for (p, blk) in emit_physical(&fext_layout, xid, &fext_addrs) {
            self.write(p, &blk[..]);
        }

        let mut vol_omap_recs = vec![omap_entry(0x405, xid, 0, im_p)];
        for (i, n) in fs_built.nodes.into_iter().enumerate() {
            let p = fs_base + i as u64;
            self.write(p, &n.block[..]);
            vol_omap_recs.push(omap_entry(n.id, xid, OMAP_VAL_NOHEADER, p));
        }
        let fs_end = fs_base + vol_omap_recs.len() as u64 - 1;
        assert!(fs_end <= 500);

        let er_built = extents::build(&self.owned);
        let leaf = |shape, p, recs: &[Record]| leaf_root(shape, p, xid, recs).unwrap();
        let nx_recs = [omap_entry(0x404, xid, 0, vol_sb)];
        for (p, blk) in [
            (vol_omap_tree, leaf(&TreeShape::OMAP, vol_omap_tree, &vol_omap_recs)),
            (vol_omap, omap_block(vol_omap, vol_omap_tree, xid, 0)),
            (im_p, integrity_block(0x405, xid, &root_hash)),
            (snap_p, leaf(&TreeShape::SNAP_META, snap_p, &[])),
            (er_p, leaf(&TreeShape::EXTENT_REF, er_p, &er_built.records)),
            (nx_omap_tree, leaf(&TreeShape::OMAP, nx_omap_tree, &nx_recs)),
            (nx_omap, omap_block(nx_omap, nx_omap_tree, xid, OMAP_MANUALLY_MANAGED)),
        ] {
            self.write(p, &blk[..]);
        }

        let mut sb = ApfsSuperblock::new_zeroed();
        sb.o = ObjPhys::new(0x404, xid, OBJ_VIRTUAL | TYPE_FS, 0);
        (sb.magic, sb.features) = (U32::new(APFS_MAGIC), U64::new(APFS_FEATURE_HARDLINK_MAP_RECORDS));
        sb.incompatible_features = U64::new(APFS_INCOMPAT_NORMALIZATION_INSENSITIVE | APFS_INCOMPAT_SEALED_VOLUME);
        sb.fs_alloc_count = U64::new((fs_end - vol_sb - 1) + er_built.blocks);
        (
            sb.root_tree_type,
            sb.extentref_tree_type,
            sb.snap_meta_tree_type,
            sb.fext_tree_type,
        ) = (
            U32::new(OBJ_VIRTUAL | TYPE_BTREE),
            U32::new(OBJ_PHYSICAL | TYPE_BTREE),
            U32::new(OBJ_PHYSICAL | TYPE_BTREE),
            U32::new(OBJ_PHYSICAL | TYPE_BTREE),
        );
        (
            sb.omap_oid,
            sb.root_tree_oid,
            sb.extentref_tree_oid,
            sb.snap_meta_tree_oid,
        ) = (U64::new(vol_omap), U64::new(0x406), U64::new(er_p), U64::new(snap_p));
        (sb.next_obj_id, sb.num_files, sb.num_directories, sb.num_symlinks) = (
            U64::new(self.next_obj_id),
            U64::new(self.num_files),
            U64::new(self.num_dirs),
            U64::new(self.num_symlinks),
        );
        (sb.total_blocks_alloced, sb.vol_uuid, sb.fs_flags) =
            (U64::new(er_built.blocks), [0xAAu8; 16], U64::new(APFS_FS_UNENCRYPTED));
        sb.volname[..6].copy_from_slice(b"Source");
        (sb.integrity_meta_oid, sb.fext_tree_oid) = (U64::new(0x405), U64::new(fext_p));
        let mut blk = [0u8; BLOCK_SIZE];
        put(&mut blk, 0, &sb);
        seal_cksum(&mut blk);
        self.write(vol_sb, &blk);

        let mut used = Bitmap::new(total_blocks);
        used.set_range(0..fs_end);
        if self.data_block > 500 {
            used.set_range(500..self.data_block);
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
            self.write(p, &sblk[..]);
        }
        for (p, eblk) in checkpoint_blocks(desc_base, data_base, xid, sp.spaceman).unwrap() {
            self.write(p, &eblk[..]);
        }

        let mut nx = NxSuperblock::new_zeroed();
        nx.o = ObjPhys::new(1, xid, OBJ_EPHEMERAL | TYPE_NX_SUPERBLOCK, 0);
        (nx.magic, nx.block_size) = (U32::new(NX_MAGIC), U32::new(BLOCK_SIZE as u32));
        (nx.block_count, nx.incompatible_features) = (U64::new(total_blocks), U64::new(NX_INCOMPAT_VERSION2));
        (nx.uuid, nx.next_oid, nx.next_xid) = ([0x11u8; 16], U64::new(oid_cur), U64::new(xid + 1));
        nx.xp_desc_blocks = U32::new(g.checkpoint.desc_blocks);
        nx.xp_data_blocks = U32::new(g.checkpoint.data_blocks);
        (nx.xp_desc_base, nx.xp_data_base) = (U64::new(desc_base), U64::new(data_base));
        (nx.xp_desc_next, nx.xp_data_next, nx.xp_desc_len, nx.xp_data_len) =
            (U32::new(2), U32::new(4), U32::new(2), U32::new(4));
        (nx.spaceman_oid, nx.omap_oid, nx.reaper_oid) = (U64::new(0x400), U64::new(nx_omap), U64::new(0x401));
        (nx.max_file_systems, nx.fs_oid[0]) = (U32::new(g.max_file_systems), U64::new(0x404));
        nx.ephemeral_info[0] = U64::new(EPHEMERAL_INFO_4096);
        blk.fill(0);
        put(&mut blk, 0, &nx);
        seal_cksum(&mut blk);
        self.write(desc_base + 1, &blk);
        self.write(0, &blk);
        self.mem
    }
}

pub fn synthetic_sealed_source() -> MemTarget {
    let mut b = Fixture::empty_root(8);
    (b.next_obj_id, b.num_files, b.num_dirs, b.num_symlinks) = (50, 5, 1, 1);

    let dir_m = InodeMeta {
        iflags: INODE_NO_RSRC_FORK,
        ..Default::default()
    };
    b.add_entry(20, ROOT_DIR_INO_NUM, "dir", DT_DIR, S_IFDIR | 0o755, dir_m);

    for (oid, name, data, extra_iflags) in [
        (16, "plain", b"hello original plain file".as_slice(), 0),
        (18, "with_xattrs", b"file with xattrs".as_slice(), INODE_HAS_SECURITY_EA),
    ] {
        let ds = b.add_stream_refcnt(oid, oid, data, Some(1));
        let m = InodeMeta {
            nlink: 1,
            iflags: INODE_NO_RSRC_FORK | extra_iflags,
            dstream: Some(ds),
            ..Default::default()
        };
        b.add_entry(oid, ROOT_DIR_INO_NUM, name, DT_REG, S_IFREG | 0o644, m);
    }

    let ds = b.add_stream_refcnt(17, 17, b"hardlinked content", Some(1));
    for (sib, name) in [(30u64, "hl1"), (31u64, "hl2")] {
        push_drec(&mut b.fs, ROOT_DIR_INO_NUM, name, 17, DT_REG, Some(sib));
        push_sibling(&mut b.fs, 17, sib, ROOT_DIR_INO_NUM, name);
    }
    let hl_m = InodeMeta {
        nlink: 2,
        iflags: INODE_NO_RSRC_FORK,
        dstream: Some(ds),
        ..Default::default()
    };
    push_inode(&mut b.fs, 17, ROOT_DIR_INO_NUM, "hl1", S_IFREG | 0o755, &hl_m);

    b.add_xattr(18, "com.apple.provenance", XATTR_DATA_EMBEDDED, b"dropme");
    b.add_xattr(18, SECURITY_XATTR_NAME, XATTR_DATA_EMBEDDED, b"aclbytes");
    let xds = XattrDstream {
        xattr_obj_id: U64::new(40),
        dstream: b.add_stream_refcnt(40, 40, &vec![0x5Au8; 6000], None),
    };
    b.add_xattr(18, "com.example.big", XATTR_DATA_STREAM | 0x10, xds.as_bytes());

    let sym_m = InodeMeta {
        nlink: 1,
        iflags: INODE_NO_RSRC_FORK,
        ..Default::default()
    };
    b.add_entry(19, ROOT_DIR_INO_NUM, "sym", DT_LNK, S_IFLNK | 0o777, sym_m);
    b.add_xattr(
        19,
        SYMLINK_XATTR_NAME,
        XATTR_DATA_EMBEDDED | XATTR_FILE_SYSTEM_OWNED,
        b"plain\0",
    );

    let comp_plain = b"inline compressed plaintext!";
    let mut decmpfs9 = decmpfs_hdr(9, comp_plain.len() as u64);
    decmpfs9.push(DECMPFS_STORED_MARKER);
    decmpfs9.extend_from_slice(comp_plain);
    b.add_entry(
        21,
        ROOT_DIR_INO_NUM,
        "comp_type9",
        DT_REG,
        S_IFREG | 0o644,
        InodeMeta {
            nlink: 1,
            bsd_flags: UF_COMPRESSED,
            iflags: INODE_NO_RSRC_FORK | INODE_HAS_UNCOMPRESSED_SIZE,
            uncompressed_size: comp_plain.len() as u64,
            ..Default::default()
        },
    );
    b.add_xattr(21, DECMPFS_XATTR_NAME, XATTR_DATA_EMBEDDED, &decmpfs9);

    let fork_plain = b"fork stored plaintext payload";
    let fds = XattrDstream {
        xattr_obj_id: U64::new(41),
        dstream: b.add_stream_refcnt(41, 22, fork_plain, None),
    };
    b.add_entry(
        22,
        ROOT_DIR_INO_NUM,
        "comp_fork",
        DT_REG,
        S_IFREG | 0o644,
        InodeMeta {
            nlink: 1,
            bsd_flags: UF_COMPRESSED,
            iflags: INODE_HAS_RSRC_FORK | INODE_HAS_UNCOMPRESSED_SIZE,
            uncompressed_size: fork_plain.len() as u64,
            ..Default::default()
        },
    );
    b.add_xattr(
        22,
        DECMPFS_XATTR_NAME,
        XATTR_DATA_EMBEDDED,
        &decmpfs_hdr(10, fork_plain.len() as u64),
    );
    b.add_xattr(22, RESOURCE_FORK_XATTR_NAME, XATTR_DATA_STREAM, fds.as_bytes());
    b.finish()
}

fn decmpfs_hdr(kind: u32, size: u64) -> Vec<u8> {
    DecmpfsHeader {
        magic: U32::new(DECMPFS_MAGIC),
        compression_type: U32::new(kind),
        uncompressed_size: U64::new(size),
    }
    .as_bytes()
    .to_vec()
}

pub(crate) fn push_drec(fs: &mut Vec<Record>, parent: u64, name: &str, file_id: u64, dtype: u16, sib: Option<u64>) {
    let mut k = JKey::new(parent, APFS_TYPE_DIR_REC).as_bytes().to_vec();
    k.extend_from_slice(&name_len_and_hash(name, UnicodeVersion::IOS_27).unwrap().to_le_bytes());
    k.extend_from_slice(name.as_bytes());
    k.push(0);
    let mut v = DrecVal {
        file_id: U64::new(file_id),
        date_added: U64::new(TEST_TIME_NS),
        flags: U16::new(dtype),
    }
    .as_bytes()
    .to_vec();
    if let Some(s) = sib {
        v.extend_from_slice(&xfields(&[XField {
            kind: DREC_EXT_TYPE_SIBLING_ID,
            flags: XF_DO_NOT_COPY,
            data: s.to_le_bytes().to_vec(),
        }]));
    }
    fs.push((k, Some(v)));
}

pub(crate) fn push_inode(fs: &mut Vec<Record>, oid: u64, parent: u64, name: &str, mode: u16, m: &InodeMeta) {
    let mut val = InodeVal::new_zeroed();
    let t = U64::new(TEST_TIME_NS);
    (val.parent_id, val.private_id) = (U64::new(parent), U64::new(m.private_id.unwrap_or(oid)));
    (val.create_time, val.mod_time, val.change_time, val.access_time) = (t, t, t, t);
    (val.internal_flags, val.nchildren_or_nlink) = (U64::new(m.iflags), I32::new(m.nlink));
    (val.write_generation_counter, val.bsd_flags) = (U32::new(2), U32::new(m.bsd_flags));
    (val.owner, val.group, val.mode) = (U32::new(501), U32::new(20), U16::new(mode));
    val.uncompressed_size = U64::new(m.uncompressed_size);
    let mut name_bytes = name.as_bytes().to_vec();
    name_bytes.push(0);
    let mut xfs = vec![XField {
        kind: INO_EXT_TYPE_NAME,
        flags: XF_DO_NOT_COPY,
        data: name_bytes,
    }];
    if let Some(d) = m.dstream {
        xfs.push(XField {
            kind: INO_EXT_TYPE_DSTREAM,
            flags: XF_SYSTEM_FIELD,
            data: d.as_bytes().to_vec(),
        });
    }
    let mut v = val.as_bytes().to_vec();
    v.extend_from_slice(&xfields(&xfs));
    fs.push((JKey::new(oid, APFS_TYPE_INODE).as_bytes().to_vec(), Some(v)));
}

fn push_sibling(fs: &mut Vec<Record>, oid: u64, sib: u64, parent_id: u64, name: &str) {
    let sk = SiblingKey {
        hdr: JKey::new(oid, APFS_TYPE_SIBLING_LINK),
        sibling_id: U64::new(sib),
    };
    let mut sv = SiblingVal {
        parent_id: U64::new(parent_id),
        name_len: U16::new(name.len() as u16 + 1),
    }
    .as_bytes()
    .to_vec();
    sv.extend_from_slice(name.as_bytes());
    sv.push(0);
    fs.push((sk.as_bytes().to_vec(), Some(sv)));
    fs.push((
        JKey::new(sib, APFS_TYPE_SIBLING_MAP).as_bytes().to_vec(),
        Some(oid.to_le_bytes().to_vec()),
    ));
}
