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
