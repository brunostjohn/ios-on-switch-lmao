// SPDX-License-Identifier: GPL-3.0-or-later
//! Trees over one node, with the shapes of Apple's own trees for records of the same sizes: the kext's
//! extent-reference trees and the sealer's fext trees of volumes with 3,004 and 20,012 files (macOS 27.2). The
//! byte-for-byte comparisons with Apple's trees run on a Mac: `//install/apfs_btree/check:btree_fixture_test`.
use std::collections::HashMap;

use sha2::{Digest, Sha256};
use switch_apfs_format::btree::{BTNODE_LEAF, BTNODE_ROOT, BtreeInfo, BtreeNodePhys};
use switch_apfs_format::{checksum_ok, get};

use super::*;

struct Read {
    flags: u16,
    level: u16,
    entries: Vec<(Vec<u8>, Option<Vec<u8>>)>,
    holes: (u16, u16),
}

fn read(shape: &TreeShape, block: &[u8]) -> Read {
    let n: BtreeNodePhys = get(block, 0).unwrap();
    let flags = n.flags.get();
    let toc = 56 + usize::from(n.table_space.off.get());
    let keys = toc + usize::from(n.table_space.len.get());
    let values_end = 4096 - if flags & BTNODE_ROOT != 0 { 40 } else { 0 };
    let leaf = flags & BTNODE_LEAF != 0;
    let mut entries = Vec::new();
    for i in 0..n.nkeys.get() as usize {
        let u16_at = |at: usize| usize::from(u16::from_le_bytes([block[at], block[at + 1]]));
        let (k, kl, v, vl) = match shape.fixed {
            Some((ks, vs)) => {
                let vl = if leaf { usize::from(vs) } else { shape.index_value_len() };
                (u16_at(toc + 4 * i), usize::from(ks), u16_at(toc + 4 * i + 2), vl)
            }
            None => (
                u16_at(toc + 8 * i),
                u16_at(toc + 8 * i + 2),
                u16_at(toc + 8 * i + 4),
                u16_at(toc + 8 * i + 6),
            ),
        };
        let key = block[keys + k..keys + k + kl].to_vec();
        let value = (v != 0xFFFF).then(|| block[values_end - v..values_end - v + vl].to_vec());
        entries.push((key, value));
    }
    Read {
        flags,
        level: n.level.get(),
        entries,
        holes: (n.key_free_list.len.get(), n.val_free_list.len.get()),
    }
}

/// Walks the tree from its root and checks it against the records: every record once, in order, each index key its
/// child's first key, levels counting down, checksums or child hashes right. Returns nkeys per node, depth first.
fn walk(shape: &TreeShape, built: &Built, records: &[Record]) -> Vec<(u16, usize)> {
    let by_id: HashMap<u64, &BuiltNode> = built.nodes.iter().map(|n| (n.id, n)).collect();
    let root_id = built.nodes[0].id;
    let mut leaves = Vec::new();
    let mut shape_seen = Vec::new();
    let mut stack = vec![(root_id, None::<Vec<u8>>, None::<u16>)];
    while let Some((id, first_key, parent_level)) = stack.pop() {
        let node = by_id[&id];
        let r = read(shape, &node.block[..]);
        assert_eq!(r.level, node.level);
        if let Some(level) = parent_level {
            assert_eq!(r.level + 1, level);
        }
        if shape.headerless() {
            assert!(node.block[..32].iter().all(|&b| b == 0));
        } else {
            assert!(checksum_ok(&node.block[..]), "node {id}");
        }
        if let Some(first) = first_key {
            assert_eq!(r.entries[0].0, first, "node {id}'s index key");
        }
        shape_seen.push((r.level, r.entries.len()));
        if r.flags & BTNODE_LEAF != 0 {
            leaves.extend(r.entries);
            continue;
        }
        for (key, value) in r.entries.into_iter().rev() {
            let value = value.unwrap();
            let child = u64::from_le_bytes(value[..8].try_into().unwrap());
            let child = if shape.hashed() { root_id + child } else { child };
            if shape.hashed() {
                let digest: [u8; 32] = Sha256::digest(&by_id[&child].block[..]).into();
                assert_eq!(&value[8..], &digest[..], "hash of {child}");
            }
            stack.push((child, Some(key), Some(r.level)));
        }
    }
    assert_eq!(leaves, records);
    let info: BtreeInfo = get(&built.nodes[0].block[..], 4096 - 40).unwrap();
    assert_eq!(info.key_count.get(), records.len() as u64);
    assert_eq!(info.node_count.get(), built.nodes.len() as u64);
    shape_seen
}

fn counter(first: u64) -> impl FnMut() -> u64 {
    let mut next = first;
    move || {
        next += 1;
        next - 1
    }
}

fn extent_refs(count: u64) -> Vec<Record> {
    (0..count)
        .map(|i| {
            let key = ((0x426 + i) | 2 << 60).to_le_bytes().to_vec();
            let mut value = (1u64 | 1 << 60).to_le_bytes().to_vec();
            value.extend_from_slice(&(0x12 + i).to_le_bytes());
            value.extend_from_slice(&1i32.to_le_bytes());
            (key, Some(value))
        })
        .collect()
}

fn fexts(count: u64) -> Vec<Record> {
    (0..count)
        .map(|i| {
            let mut key = (0x11 + i).to_le_bytes().to_vec();
            key.extend_from_slice(&0u64.to_le_bytes());
            let mut value = 4096u64.to_le_bytes().to_vec();
            value.extend_from_slice(&(0x1000 + i).to_le_bytes());
            (key, Some(value))
        })
        .collect()
}

fn leaf_counts(seen: &[(u16, usize)], level: u16) -> Vec<usize> {
    seen.iter().filter(|s| s.0 == level).map(|s| s.1).collect()
}

#[test]
fn kext_extent_reference_tree_of_3004_files() {
    let records = extent_refs(3004);
    let built = build(&TreeShape::EXTENT_REF, 3, &records, &mut counter(1063)).unwrap();
    let seen = walk(&TreeShape::EXTENT_REF, &built, &records);
    let mut leaves = vec![110];
    leaves.extend([112; 25]);
    leaves.push(94);
    assert_eq!(leaf_counts(&seen, 0), leaves);
    assert_eq!(seen[0], (1, 27));
}

/// The root splits with 164 entries on its left: a root takes no slack.
#[test]
fn kext_extent_reference_tree_of_20012_files() {
    let records = extent_refs(20012);
    let built = build(&TreeShape::EXTENT_REF, 4, &records, &mut counter(16741)).unwrap();
    let seen = walk(&TreeShape::EXTENT_REF, &built, &records);
    assert_eq!(built.nodes.len(), 182);
    assert_eq!(leaf_counts(&seen, 1), vec![164, 15]);
    assert_eq!(seen[0], (2, 2));
}

#[test]
fn sealer_fext_tree_of_20012_files() {
    let records = fexts(20012);
    let built = build(&TreeShape::FEXT, 6, &records, &mut counter(22865)).unwrap();
    let seen = walk(&TreeShape::FEXT, &built, &records);
    assert_eq!(built.nodes.len(), 182);
    assert_eq!(leaf_counts(&seen, 1), vec![140, 39]);
    let leaves = leaf_counts(&seen, 0);
    assert_eq!((leaves[0], leaves[1], leaves[leaves.len() - 1]), (111, 112, 77));
    let table = u16::from_le_bytes([built.nodes[0].block[0x2a], built.nodes[0].block[0x2b]]);
    assert_eq!(table, 144 * 4);
}

/// One record per leaf, so the index level splits often. Each split of a non-root index node leaves its moved entry
/// behind as a hole: an `nloc_t` over its first bytes, the rest as it was, the hash never filled in. Only the left node
/// of the root's split and the last node have none.
#[test]
fn sealed_index_splits_leave_holes() {
    let records: Vec<Record> = (0..400u64)
        .map(|i| ((i << 8 | 3 << 60).to_le_bytes().to_vec(), Some(vec![i as u8; 3000])))
        .collect();
    let shape = TreeShape::SEALED_FS_TREE;
    let built = build(&shape, 0, &records, &mut counter(1266)).unwrap();
    let root_hash: [u8; 32] = Sha256::digest(&built.nodes[0].block[..]).into();
    assert_eq!(built.root_hash, Some(root_hash));
    walk(&shape, &built, &records);
    let index: Vec<&BuiltNode> = built.nodes[1..].iter().filter(|n| n.level == 1).collect();
    assert!(index.len() > 3);
    let with_holes: Vec<Read> = index
        .iter()
        .map(|n| read(&shape, &n.block[..]))
        .filter(|r| r.holes != (0, 0))
        .collect();
    assert_eq!(with_holes.len(), index.len() - 2);
    for node in &built.nodes[1..] {
        let r = read(&shape, &node.block[..]);
        if r.holes == (0, 0) {
            continue;
        }
        assert_eq!(r.holes, (8, 40));
        let b = &node.block[..];
        let kfl = usize::from(u16::from_le_bytes([b[0x30], b[0x31]]));
        let vfl = usize::from(u16::from_le_bytes([b[0x34], b[0x35]]));
        let keys = 56 + usize::from(u16::from_le_bytes([b[0x2a], b[0x2b]]));
        assert_eq!(&b[keys + kfl..keys + kfl + 4], &[0xff, 0xff, 8, 0]);
        assert_eq!(b[keys + kfl + 7] >> 4, 3);
        assert_eq!(&b[4096 - vfl..4096 - vfl + 4], &[0xff, 0xff, 40, 0]);
        assert!(b[4096 - vfl + 8..4096 - vfl + 40].iter().all(|&x| x == 0));
    }
}

#[test]
fn same_input_same_bytes() {
    let records = extent_refs(5000);
    let a = build(&TreeShape::EXTENT_REF, 9, &records, &mut counter(100)).unwrap();
    let b = build(&TreeShape::EXTENT_REF, 9, &records, &mut counter(100)).unwrap();
    assert!(
        a.nodes
            .iter()
            .zip(&b.nodes)
            .all(|(x, y)| x.id == y.id && x.block == y.block)
    );
}

#[test]
fn layout_counts_before_ids() {
    let records = fexts(20012);
    let layout = Layout::new(&TreeShape::FEXT, &records).unwrap();
    assert_eq!((layout.node_count(), layout.height()), (182, 3));
    let mut calls = 0;
    layout.emit(1, &mut || {
        calls += 1;
        calls
    });
    assert_eq!(calls, 182);
}

#[test]
fn single_node_trees_still_fit_one_leaf() {
    let records = extent_refs(110);
    assert!(leaf_root(&TreeShape::EXTENT_REF, 1, 1, &records).is_ok());
    let records = extent_refs(111);
    assert!(matches!(
        leaf_root(&TreeShape::EXTENT_REF, 1, 1, &records),
        Err(BuildError::Overflow { .. })
    ));
}

#[test]
fn refuses_a_record_no_node_holds() {
    let records = vec![(vec![1; 8], Some(vec![0; 4000]))];
    assert_eq!(
        build(&TreeShape::FS_TREE, 1, &records, &mut counter(1)).err(),
        Some(BuildError::TooBig(0))
    );
}

#[test]
fn empty_ephemeral_root() {
    let block = leaf_root(&TreeShape::FREE_QUEUE, 1027, 7, &[]).unwrap();
    let n: BtreeNodePhys = get(&block[..], 0).unwrap();
    assert_eq!(n.o.type_and_flags.get(), 0x8000_0002);
    assert_eq!((n.flags.get(), n.nkeys.get()), (0x7, 0));
    assert_eq!(
        (n.table_space.len.get(), n.free_space.off.get(), n.free_space.len.get()),
        (144 * 4, 0, 3424)
    );
    let info: BtreeInfo = get(&block[..], 4096 - 40).unwrap();
    assert_eq!((info.key_count.get(), info.node_count.get()), (0, 1));
    assert!(checksum_ok(&block[..]));
}

/// One record per leaf and index keys of 8 and 700 bytes, so index nodes hold a handful of entries: the tree grows to
/// four levels or more, and index nodes split under non-root parents.
#[test]
fn deep_trees_with_mixed_keys() {
    let records: Vec<Record> = (0..3000u64)
        .map(|i| {
            let mut key = i.to_be_bytes().to_vec();
            if i % 2 == 1 {
                key.resize(700, 0x5a);
            }
            (key, Some(vec![i as u8; 3000]))
        })
        .collect();
    for shape in [TreeShape::FS_TREE, TreeShape::SEALED_FS_TREE] {
        let built = build(&shape, 4, &records, &mut counter(5000)).unwrap();
        let seen = walk(&shape, &built, &records);
        assert!(seen[0].0 >= 3, "{shape:?}: {} levels", seen[0].0 + 1);
        let upper_holes = built.nodes[1..]
            .iter()
            .filter(|n| n.level >= 2 && read(&shape, &n.block[..]).holes != (0, 0))
            .count();
        assert!(upper_holes > 0, "{shape:?}");
    }
}

/// Key lengths from 8 to 1,000 bytes in a fixed pseudo-random order, so an entry a split moves up is often longer than
/// the key its parent was checked against.
#[test]
fn deep_trees_with_random_key_lengths() {
    let mut state = 0x2545_f491_4f6c_dd1du64;
    let records: Vec<Record> = (0..6000u64)
        .map(|i| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let mut key = i.to_be_bytes().to_vec();
            key.resize(8 + (state % 993) as usize, 0xa5);
            (key, Some(vec![i as u8; 2900]))
        })
        .collect();
    for shape in [TreeShape::FS_TREE, TreeShape::SEALED_FS_TREE] {
        let built = build(&shape, 4, &records, &mut counter(9000)).unwrap();
        let seen = walk(&shape, &built, &records);
        assert!(seen[0].0 >= 3, "{shape:?}: {} levels", seen[0].0 + 1);
    }
}
