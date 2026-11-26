// SPDX-License-Identifier: GPL-3.0-or-later
//! Trees and sources the writer must refuse rather than write, and the replaced content it must leave out.

use std::collections::BTreeMap;
use std::sync::Arc;

use switch_apfs_alloc::Bitmap;
use switch_apfs_format::container::{CheckpointMapping, NxSuperblock};
use switch_apfs_format::object::{OBJ_EPHEMERAL, ObjPhys, TYPE_NX_REAP_LIST};
use switch_apfs_format::{BLOCK_SIZE, put, seal};
use switch_apfs_read::Checkpoint;
use zerocopy::FromZeros;
use zerocopy::little_endian::{U32, U64};

use crate::checkpoint::{self, CheckpointInput};
use crate::content::{Content, SourceExtent};
use crate::place::{FreeRuns, place};
use crate::records::{self, RecordInput};
use crate::tests::dense;
use crate::tree::{Attrs, Tree, Xattr, XattrValue};

fn try_build(tree: &Tree, extents: &[Vec<crate::place::PlacedExtent>], incompat: u64) -> bool {
    records::build(&RecordInput {
        tree,
        extents,
        incompatible_features: incompat,
        readonly_compatible_features: 0xc,
        unicode: switch_apfs_names::UnicodeVersion::IOS_27,
        time_ns: 0,
    })
    .is_ok()
}

fn file() -> Attrs {
    Attrs::new(0, 0, 0o644)
}

#[test]
fn two_xattrs_naming_one_stream_are_refused() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let s = tree.add_bytes(vec![9u8; 8000]);
    for name in ["a", "b"] {
        let f = tree.add_file(tree.root(), name, file(), None).unwrap();
        tree.put_xattr(
            f,
            Xattr {
                name: "com.example.big".into(),
                flags: 0x10,
                value: XattrValue::Stream(s),
            },
        );
    }
    assert!(
        !try_build(&tree, &[dense(100, 2)], 0x208),
        "two xattrs on one stream were planned"
    );
}

#[test]
fn replaced_content_leaves_the_old_streams_out() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let old = tree.add_bytes(&b"old\n"[..]);
    let plain = tree.add_file(tree.root(), "plain", file(), Some(old)).unwrap();
    let compressed = tree.add_file(tree.root(), "compressed", file(), None).unwrap();
    let fork = tree.add_bytes(vec![7u8; 5000]);
    tree.put_xattr(
        compressed,
        Xattr {
            name: "com.apple.ResourceFork".into(),
            flags: 0,
            value: XattrValue::Stream(fork),
        },
    );
    let new = tree.add_bytes(&b"new\n"[..]);
    let other = tree.add_bytes(&b"other\n"[..]);
    tree.replace_content(plain, Some(new)).unwrap();
    tree.replace_content(compressed, Some(other)).unwrap();
    let r = records::build(&RecordInput {
        tree: &tree,
        extents: &[dense(100, 1), dense(101, 2), dense(103, 1), dense(104, 1)],
        incompatible_features: 0x208,
        readonly_compatible_features: 0xc,
        unicode: switch_apfs_names::UnicodeVersion::IOS_27,
        time_ns: 0,
    });
    let r = r.unwrap_or_else(|e| panic!("W5's replace_content can't be planned: {e}"));
    let refs: Vec<u64> = r
        .extent_refs
        .iter()
        .map(|(k, _)| u64::from_le_bytes(k[..8].try_into().unwrap()) & 0x0fff_ffff_ffff_ffff)
        .collect();
    assert_eq!(refs, [103, 104], "only the new streams' blocks are referenced");

    let mut used = Bitmap::new(1000);
    used.set_range(0..10);
    let mut source = Tree::new(Attrs::new(0, 0, 0o755));
    let extent = SourceExtent {
        logical: 0,
        len: BLOCK_SIZE as u64,
        flags: 0,
        paddr: 500,
        crypto_id: 0,
    };
    let copied = source.add_stream(BLOCK_SIZE as u64, Content::Source(Arc::from([extent])));
    let f = source.add_file(source.root(), "f", file(), Some(copied)).unwrap();
    let new = source.add_bytes(&b"new\n"[..]);
    source.replace_content(f, Some(new)).unwrap();
    let p = place(&[&source], &mut FreeRuns::from_bitmap(&used)).unwrap();
    assert!(p.relocations.is_empty(), "the replaced source stream is still copied");
}

#[test]
fn names_one_normalisation_apart_are_one_name() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let nfc = tree.add_file(tree.root(), "caf\u{e9}", file(), None).unwrap();
    assert!(
        tree.add_file(tree.root(), "cafe\u{301}", file(), None).is_err(),
        "the NFD form of an existing NFC name was added"
    );
    assert_eq!(tree.lookup("cafe\u{301}"), Some(nfc));
    assert_eq!(tree.lookup("caf\u{e9}"), Some(nfc));
}

#[test]
fn a_source_block_two_volumes_copy_is_refused() {
    let mut used = Bitmap::new(1000);
    used.set_range(0..10);
    let tree = |paddr| {
        let mut t = Tree::new(Attrs::new(0, 0, 0o755));
        let e = SourceExtent {
            logical: 0,
            len: 2 * BLOCK_SIZE as u64,
            flags: 0,
            paddr,
            crypto_id: 0,
        };
        let s = t.add_stream(2 * BLOCK_SIZE as u64, Content::Source(Arc::from([e])));
        t.add_file(t.root(), "f", file(), Some(s)).unwrap();
        t
    };
    let (a, b) = (tree(500), tree(501));
    assert!(
        place(&[&a, &b], &mut FreeRuns::from_bitmap(&used)).is_err(),
        "block 501 was copied into two volumes"
    );
    let c = tree(502);
    assert!(place(&[&a, &c], &mut FreeRuns::from_bitmap(&used)).is_ok());
}

#[test]
fn a_case_insensitive_volume_is_refused() {
    let tree = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(!try_build(&tree, &[], 0x209));
    assert!(try_build(&tree, &[], 0x208));
}

#[test]
fn source_extents_that_overlap_or_pass_the_stream_end_are_refused() {
    let mut used = Bitmap::new(1000);
    used.set_range(0..10);
    let bs = BLOCK_SIZE as u64;
    let at = |logical: u64, blocks: u64, paddr: u64| SourceExtent {
        logical,
        len: blocks * bs,
        flags: 0,
        paddr,
        crypto_id: 0,
    };
    let try_place = |size: u64, extents: Vec<SourceExtent>| {
        let mut t = Tree::new(Attrs::new(0, 0, 0o755));
        let s = t.add_stream(size, Content::Source(Arc::from(extents)));
        t.add_file(t.root(), "f", file(), Some(s)).unwrap();
        place(&[&t], &mut FreeRuns::from_bitmap(&used)).is_ok()
    };
    assert!(try_place(3 * bs, vec![at(0, 2, 500), at(2 * bs, 1, 0)]));
    assert!(try_place(3 * bs - 100, vec![at(0, 3, 500)]));
    assert!(
        !try_place(3 * bs, vec![at(0, 2, 500), at(bs, 2, 600)]),
        "overlapping extents"
    );
    assert!(
        !try_place(2 * bs, vec![at(0, 2, 500), at(2 * bs, 1, 600)]),
        "an extent past the end"
    );
    assert!(!try_place(bs, vec![at(0, 2, 500)]), "an extent ending past the end");
}

#[test]
fn a_reap_list_with_work_left_is_refused() {
    let mut list = vec![0u8; BLOCK_SIZE];
    put(
        &mut list,
        0,
        &ObjPhys::new(1030, 3, OBJ_EPHEMERAL | TYPE_NX_REAP_LIST, 0),
    );
    put(&mut list, 48, &U32::new(1));
    seal(&mut list);
    let mut block = Box::new([0u8; BLOCK_SIZE]);
    block.copy_from_slice(&list);
    let mapping = CheckpointMapping {
        type_and_flags: U32::new(OBJ_EPHEMERAL | TYPE_NX_REAP_LIST),
        subtype: U32::ZERO,
        size: U32::new(BLOCK_SIZE as u32),
        pad: U32::ZERO,
        fs_oid: U64::ZERO,
        oid: U64::new(1030),
        paddr: U64::new(20),
    };
    let nx = NxSuperblock::new_zeroed();
    let source = Checkpoint {
        nx,
        nx_block: Box::new([0u8; BLOCK_SIZE]),
        nx_paddr: 11,
        block_zero: Box::new([0u8; BLOCK_SIZE]),
        desc_area: 10..12,
        data_area: 20..28,
        mappings: vec![mapping],
        ephemeral: BTreeMap::from([(1030, (20, block))]),
    };
    let r = checkpoint::build(CheckpointInput {
        source: &source,
        xid: 4,
        desc: 40..48,
        data: 50..60,
        spaceman: &[0u8; BLOCK_SIZE],
        free_queue_oids: [1, 2],
        nx,
    });
    assert!(r.is_err(), "a reap list holding one entry was carried over");
}
