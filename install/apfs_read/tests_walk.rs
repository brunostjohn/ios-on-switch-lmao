// SPDX-License-Identifier: GPL-3.0-or-later
//! The sealed fixture's fs-tree nodes from the root to the leaf holding `/data/d4`, as apfs_prepare_cryptex wrote
//! them (`switch_apfs_read_tool capture`), and stream reads over holes.

use std::collections::HashMap;
use std::io;

use switch_apfs_format::io::BlockSource;
use switch_apfs_format::{BLOCK_SIZE, Block};

use crate::catalog::{self, FileExtent, Record, XattrData};
use crate::node::Node;
use crate::subtree::Stream;
use crate::tests_records::unhex;
use crate::walk::{self, NodeReader, Tree};
use crate::{ReadError, stream};

const ROOT: u64 = 1189;
const ROOT_HASH: &str = "01ecf35feb4a5c21d693f7377b215acc81497e3e70206772d1a220d2fa1ead6b";
const D4: u64 = 3032;
const NODES: [(u64, &[u8; BLOCK_SIZE]); 3] = [
    (1189, include_bytes!("testdata/sealed-d4/0-root-1189.blk")),
    (1253, include_bytes!("testdata/sealed-d4/1-index-1253.blk")),
    (1321, include_bytes!("testdata/sealed-d4/2-leaf-1321.blk")),
];

struct Mem(HashMap<u64, Box<Block>>);

impl Mem {
    fn sealed() -> Self {
        Mem(NODES.iter().map(|(oid, b)| (*oid, Box::new(**b))).collect())
    }
}

impl NodeReader for Mem {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError> {
        let block = self
            .0
            .get(&oid)
            .ok_or_else(|| ReadError::invalid(format!("node {oid} not captured")))?;
        Ok((oid, block.clone()))
    }
}

fn d4_records(nodes: &Mem, root_hash: [u8; 32]) -> Result<Vec<Record>, ReadError> {
    let tree = Tree {
        reader: nodes,
        root: ROOT,
        root_hash: Some(root_hash),
    };
    let mut out = Vec::new();
    let range = |key: &[u8]| catalog::order(key, D4, None);
    walk::scan(&tree, Some(&range), &mut |k, v| {
        out.push(Record::decode(k, v.unwrap(), true)?);
        Ok(())
    })?;
    Ok(out)
}

fn root_hash() -> [u8; 32] {
    unhex(ROOT_HASH).try_into().unwrap()
}

#[test]
fn sealed_nodes_are_headerless_and_hashed() {
    let root = Node::parse(NODES[0].1, None).unwrap();
    assert!(root.is_headerless() && root.is_hashed() && !root.is_leaf());
    assert_eq!(root.level(), 2);
    assert_eq!(root.info.map(|i| i.flags.get()), Some(0x1c2));
    let leaf = Node::parse(NODES[2].1, None).unwrap();
    assert!(leaf.is_headerless() && leaf.is_hashed() && leaf.is_leaf());
}

#[test]
fn a_sealed_object_reads_through_its_hashed_chain() {
    let records = d4_records(&Mem::sealed(), root_hash()).unwrap();
    let [
        Record::Inode(inode),
        Record::Xattr(big),
        Record::DstreamId { refcnt: 1, .. },
        Record::FileInfo(info),
    ] = records.as_slice()
    else {
        panic!("{records:?}")
    };
    assert_eq!(
        (inode.name().as_deref(), inode.val.owner.get(), inode.val.group.get()),
        (Some("d4"), 501, 20)
    );
    assert_eq!(inode.dstream().map(|d| d.size.get()), Some(7));
    assert_eq!(inode.val.mod_time.get(), inode.val.create_time.get());
    assert_eq!((big.name.as_str(), big.flags), ("com.example.big", 0x1));
    assert!(matches!(big.data, XattrData::Stream { id: 0xc71, .. }));
    assert_eq!(
        (info.stream, info.info_type, info.offset, info.hashed_len),
        (D4, 1, 0, 1)
    );
    assert_eq!(info.hash[..4], [0x9a, 0xab, 0x91, 0x2f]);
}

#[test]
fn a_sealed_chain_refuses_any_changed_byte() {
    assert!(d4_records(&Mem::sealed(), [0; 32]).is_err());
    for (oid, at) in [(1189, 3000), (1253, 2000), (1321, 1000), (1321, 33)] {
        let mut nodes = Mem::sealed();
        nodes.0.get_mut(&oid).unwrap()[at] ^= 1;
        let e = d4_records(&nodes, root_hash()).unwrap_err().to_string();
        assert!(e.contains(&format!("node {oid}")), "{oid}+{at}: {e}");
    }
}

struct Blocks(HashMap<u64, u8>);

impl BlockSource for Blocks {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
        for (i, chunk) in out.chunks_mut(BLOCK_SIZE).enumerate() {
            chunk.fill(*self.0.get(&(paddr + i as u64)).ok_or(io::ErrorKind::UnexpectedEof)?);
        }
        Ok(())
    }
}

fn extent(logical: u64, blocks: u64, phys: u64) -> FileExtent {
    FileExtent {
        stream: 7,
        logical_addr: logical * BLOCK_SIZE as u64,
        len: blocks * BLOCK_SIZE as u64,
        flags: 0,
        phys_block_num: phys,
        crypto_id: 0,
    }
}

#[test]
fn streams_read_in_logical_order_with_holes_as_zeros() {
    let src = Blocks(HashMap::from([(100, 0xa1), (101, 0xa2), (300, 0xb1)]));
    let s = Stream {
        id: 7,
        size: 4 * BLOCK_SIZE as u64 + 10,
        extents: vec![extent(4, 1, 300), extent(0, 2, 100), extent(2, 2, 0)],
        file_info: Vec::new(),
        refcnt: None,
    };
    let mut out = Vec::new();
    stream::read(&src, &s, &mut |c| out.extend_from_slice(c)).unwrap();
    let firsts: Vec<u8> = out.chunks(BLOCK_SIZE).map(|c| c[0]).collect();
    assert_eq!((out.len(), firsts), (4 * BLOCK_SIZE + 10, vec![0xa1, 0xa2, 0, 0, 0xb1]));

    let overlapping = Stream {
        extents: vec![extent(0, 2, 100), extent(1, 1, 300)],
        ..s
    };
    assert!(stream::read(&src, &overlapping, &mut |_| {}).is_err());
}

#[test]
fn entry_accepts_empty_clone_owner_with_reassigned_private_id() {
    use switch_apfs_format::btree::{
        BTNODE_FIXED_KV_SIZE, BTNODE_LEAF, BTNODE_ROOT, BTREE_INFO_SIZE, BTREE_NODE_DATA_AT, BtreeInfo, BtreeNodePhys,
        Kvloc, Kvoff, Nloc,
    };
    use switch_apfs_format::fs::{
        APFS_TYPE_DSTREAM_ID, APFS_TYPE_INODE, Dstream, DstreamIdVal, INO_EXT_TYPE_DSTREAM, INODE_NO_RSRC_FORK,
        InodeVal, JKey, S_IFREG, XF_SYSTEM_FIELD, XfBlob,
    };
    use switch_apfs_format::object::{OBJ_PHYSICAL, OBJ_VIRTUAL, ObjPhys, TYPE_BTREE, TYPE_FS, TYPE_OMAP};
    use switch_apfs_format::omap::{OmapKey, OmapPhys, OmapVal};
    use switch_apfs_format::volume::{APFS_FS_UNENCRYPTED, APFS_MAGIC, ApfsSuperblock};
    use switch_apfs_format::{put, seal};
    use zerocopy::little_endian::{I32, U16, U32, U64};
    use zerocopy::{FromZeros, IntoBytes};

    use crate::fs::Fs;
    use crate::omap::Volume;

    struct BlockMap(HashMap<u64, Box<Block>>);
    impl BlockSource for BlockMap {
        fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
            for (i, ch) in out.chunks_mut(BLOCK_SIZE).enumerate() {
                let blk = self.0.get(&(paddr + i as u64)).ok_or(io::ErrorKind::UnexpectedEof)?;
                ch.copy_from_slice(&blk[..]);
            }
            Ok(())
        }
    }

    fn leaf(oid: u64, obj_type: u32, fixed: Option<(u32, u32)>, recs: &[(Vec<u8>, Vec<u8>)]) -> Box<Block> {
        let mut blk = Box::new([0u8; BLOCK_SIZE]);
        let flags = BTNODE_ROOT | BTNODE_LEAF | if fixed.is_some() { BTNODE_FIXED_KV_SIZE } else { 0 };
        let toc_entry = if fixed.is_some() {
            size_of::<Kvoff>()
        } else {
            size_of::<Kvloc>()
        };
        let toc_len = recs.len() * toc_entry;
        let key_area = BTREE_NODE_DATA_AT + toc_len;
        let values_end = BLOCK_SIZE - BTREE_INFO_SIZE;
        let mut kcur = 0usize;
        let mut vcur = 0usize;
        for (i, (k, v)) in recs.iter().enumerate() {
            blk[key_area + kcur..key_area + kcur + k.len()].copy_from_slice(k);
            vcur += v.len();
            blk[values_end - vcur..values_end - vcur + v.len()].copy_from_slice(v);
            let at = BTREE_NODE_DATA_AT + i * toc_entry;
            if fixed.is_some() {
                put(
                    &mut blk[..],
                    at,
                    &Kvoff {
                        k: U16::new(kcur as u16),
                        v: U16::new(vcur as u16),
                    },
                );
            } else {
                put(
                    &mut blk[..],
                    at,
                    &Kvloc {
                        k: Nloc {
                            off: U16::new(kcur as u16),
                            len: U16::new(k.len() as u16),
                        },
                        v: Nloc {
                            off: U16::new(vcur as u16),
                            len: U16::new(v.len() as u16),
                        },
                    },
                );
            }
            kcur += k.len();
        }
        put(
            &mut blk[..],
            0,
            &BtreeNodePhys {
                o: ObjPhys::new(oid, 1, obj_type, 0),
                flags: U16::new(flags),
                level: U16::ZERO,
                nkeys: U32::new(recs.len() as u32),
                table_space: Nloc {
                    off: U16::ZERO,
                    len: U16::new(toc_len as u16),
                },
                free_space: Nloc::default(),
                key_free_list: Nloc::default(),
                val_free_list: Nloc::default(),
            },
        );
        put(
            &mut blk[..],
            values_end,
            &BtreeInfo {
                flags: U32::ZERO,
                node_size: U32::new(BLOCK_SIZE as u32),
                key_size: U32::new(fixed.map_or(0, |f| f.0)),
                val_size: U32::new(fixed.map_or(0, |f| f.1)),
                longest_key: U32::new(fixed.map_or(0, |f| f.0)),
                longest_val: U32::new(fixed.map_or(0, |f| f.1)),
                key_count: U64::new(recs.len() as u64),
                node_count: U64::new(1),
            },
        );
        seal(&mut blk[..]);
        blk
    }

    fn inode_val(private_id: u64, size: Option<u64>) -> Vec<u8> {
        let mut v = InodeVal::new_zeroed();
        v.parent_id = U64::new(2);
        v.private_id = U64::new(private_id);
        v.internal_flags = U64::new(INODE_NO_RSRC_FORK);
        v.nchildren_or_nlink = I32::new(1);
        v.mode = U16::new(S_IFREG | 0o644);
        let mut out = v.as_bytes().to_vec();
        if let Some(sz) = size {
            let ds = Dstream {
                size: U64::new(sz),
                alloced_size: U64::new(BLOCK_SIZE as u64),
                ..Default::default()
            };
            let blob = XfBlob {
                num_exts: U16::new(1),
                used_data: U16::new(size_of::<Dstream>() as u16),
            };
            out.extend_from_slice(blob.as_bytes());
            out.extend_from_slice(&[INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD]);
            out.extend_from_slice(&(size_of::<Dstream>() as u16).to_le_bytes());
            out.extend_from_slice(ds.as_bytes());
        }
        out
    }

    let make_fs = |inode16_private_id: u64| -> (BlockMap, Volume) {
        let fs_recs = vec![
            (
                JKey::new(16, APFS_TYPE_INODE).as_bytes().to_vec(),
                inode_val(inode16_private_id, None),
            ),
            (
                JKey::new(16, APFS_TYPE_DSTREAM_ID).as_bytes().to_vec(),
                DstreamIdVal { refcnt: U32::new(1) }.as_bytes().to_vec(),
            ),
            (
                JKey::new(17, APFS_TYPE_INODE).as_bytes().to_vec(),
                inode_val(16, Some(100)),
            ),
        ];
        let omap_recs = vec![(
            OmapKey {
                oid: U64::new(0x406),
                xid: U64::new(1),
            }
            .as_bytes()
            .to_vec(),
            OmapVal {
                flags: U32::ZERO,
                size: U32::new(BLOCK_SIZE as u32),
                paddr: U64::new(12),
            }
            .as_bytes()
            .to_vec(),
        )];
        let mut omap_blk = Box::new([0u8; BLOCK_SIZE]);
        put(
            &mut omap_blk[..],
            0,
            &OmapPhys {
                o: ObjPhys::new(10, 1, OBJ_PHYSICAL | TYPE_OMAP, 0),
                tree_type: U32::new(OBJ_PHYSICAL | TYPE_BTREE),
                snapshot_tree_type: U32::new(OBJ_PHYSICAL | TYPE_BTREE),
                tree_oid: U64::new(11),
                ..Default::default()
            },
        );
        seal(&mut omap_blk[..]);

        let mut sb = ApfsSuperblock::new_zeroed();
        sb.o = ObjPhys::new(0x404, 1, OBJ_VIRTUAL | TYPE_FS, 0);
        sb.magic = U32::new(APFS_MAGIC);
        sb.root_tree_type = U32::new(OBJ_VIRTUAL | TYPE_BTREE);
        sb.omap_oid = U64::new(10);
        sb.root_tree_oid = U64::new(0x406);
        sb.fs_flags = U64::new(APFS_FS_UNENCRYPTED);
        let vol = Volume {
            index: 0,
            oid: 0x404,
            xid: 1,
            paddr: 9,
            sb,
            block: Box::new([0u8; BLOCK_SIZE]),
        };
        let mut blocks = HashMap::new();
        blocks.insert(10, omap_blk);
        blocks.insert(11, leaf(11, OBJ_PHYSICAL | TYPE_BTREE, Some((16, 16)), &omap_recs));
        blocks.insert(12, leaf(0x406, OBJ_VIRTUAL | TYPE_BTREE, None, &fs_recs));
        (BlockMap(blocks), vol)
    };

    // Apple's `cp -c a b; : > a`: empty inode 16 has private_id 18 while clone 17 owns stream 16.
    let (src, vol) = make_fs(18);
    let fs = Fs::open(&src, &vol).unwrap();
    let (entry_a, _) = fs.entry(16, "/a".into(), None).unwrap();
    assert!(entry_a.data.is_none());
    let (entry_b, _) = fs.entry(17, "/b".into(), None).unwrap();
    assert_eq!(entry_b.data.as_ref().and_then(|s| s.refcnt), Some(1));

    // A corrupt empty inode 16 that still claims private_id 16 must still be rejected.
    let (bad_src, bad_vol) = make_fs(16);
    let bad_fs = Fs::open(&bad_src, &bad_vol).unwrap();
    let err = bad_fs.entry(16, "/a".into(), None).unwrap_err().to_string();
    assert!(err.contains("stream records without a dstream"), "got: {err}");
}
