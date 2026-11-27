// SPDX-License-Identifier: GPL-3.0-or-later
//! Records compared with the ones macOS 27.2's kext wrote for the same files on a Data-role volume with the spec's
//! feature bits. Times and the write-generation counter differ by nature.

use switch_apfs_names::UnicodeVersion;

use crate::place::PlacedExtent;
use crate::records::{self, RecordInput, Records};
use crate::tree::{Attrs, Tree};

pub fn hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

pub fn le(v: u64) -> String {
    v.to_le_bytes().iter().map(|b| format!("{b:02x}")).collect()
}

pub fn dense(paddr: u64, blocks: u64) -> Vec<PlacedExtent> {
    vec![PlacedExtent {
        logical: 0,
        len: blocks * 4096,
        flags: 0,
        paddr,
        crypto_id: 0,
    }]
}

pub fn build(tree: &Tree, extents: &[Vec<PlacedExtent>], incompat: u64, rocompat: u64) -> Records {
    records::build(&RecordInput {
        tree,
        extents,
        incompatible_features: incompat,
        readonly_compatible_features: rocompat,
        unicode: UnicodeVersion::IOS_27,
        time_ns: 0,
    })
    .unwrap()
}

pub fn value(r: &Records, key: &str) -> Vec<u8> {
    r.fs.iter().find(|(k, _)| k == &hex(key)).unwrap().1.clone().unwrap()
}

fn hello_tree() -> Tree {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let data = tree.add_bytes(&b"hello\n"[..]);
    tree.add_file(tree.root(), "hello", Attrs::new(0, 0, 0o644), Some(data))
        .unwrap();
    tree
}

/// Zeroes an inode value's four times and its write-generation counter.
fn mask_inode(value: &[u8]) -> Vec<u8> {
    let mut v = value.to_vec();
    v[16..48].fill(0);
    v[64..68].fill(0);
    v
}

#[test]
fn hello_records_match_macos() {
    let tree = hello_tree();
    let r = build(&tree, &[dense(0xcb, 1)], 0x208, 0xc);
    let keys: Vec<String> =
        r.fs.iter()
            .map(|(k, _)| k.iter().map(|b| format!("{b:02x}")).collect())
            .collect();
    assert_eq!(
        keys,
        [
            "01000000000000900c8ca6ac707269766174652d64697200",
            "010000000000009005e471b6726f6f7400",
            "0200000000000030",
            "020000000000009006ccaec768656c6c6f00",
            "0300000000000030",
            "1000000000000030",
            "1000000000000060",
            "10000000000000800000000000000000",
        ]
    );

    let apple = hex(concat!(
        "02000000000000001200000000000000",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0080000000000000",
        "01000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "a481",
        "0000",
        "0000000000000000",
        "020030000402060008202800",
        "68656c6c6f000000",
        "0600000000000000",
        "0010000000000000",
        "0000000000000000",
        "0600000000000000",
        "0000000000000000",
    ));
    let mut ours = mask_inode(&value(&r, "1000000000000030"));
    ours[8..16].copy_from_slice(&0x12u64.to_le_bytes());
    assert_eq!(ours, apple);

    let apple_private = hex(concat!(
        "01000000000000000300000000000000",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0080000000000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
        "a441",
        "0000",
        "0000000000000000",
        "0100100004020c00",
        "707269766174652d6469720000000000",
    ));
    assert_eq!(mask_inode(&value(&r, "0300000000000030")), apple_private);

    let root = value(&r, "0200000000000030");
    assert_eq!(&root[56..60], &1i32.to_le_bytes(), "one child");
    assert_eq!(&root[84..92], &0u64.to_le_bytes(), "no subdirectories");
    assert_eq!(&root[92..], &hex("0100080004020500726f6f7400000000")[..]);

    assert_eq!(
        value(&r, "10000000000000800000000000000000"),
        hex("0010000000000000cb000000000000000000000000000000")
    );
    assert_eq!(
        r.extent_refs,
        vec![(
            hex("cb00000000000020"),
            Some(hex("0100000000000010100000000000000001000000"))
        )]
    );
    assert_eq!((r.files, r.dirs, r.data_blocks), (1, 0, 1));
}

#[test]
fn subdirectory_count_follows_the_feature() {
    let mut tree = hello_tree();
    tree.add_dir(tree.root(), "sub", Attrs::new(0, 0, 0o755)).unwrap();
    for (incompat, expected) in [(0x208u64, 1u64), (0x8, 0)] {
        let r = build(&tree, &[dense(0xcb, 1)], incompat, 0);
        let root = value(&r, "0200000000000030");
        assert_eq!(u64::from_le_bytes(root[84..92].try_into().unwrap()), expected);
        assert_eq!(i32::from_le_bytes(root[56..60].try_into().unwrap()), 2);
    }
}

#[test]
fn new_names_outside_the_unicode_version_are_refused() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    tree.add_file(tree.root(), "\u{1ACF}", Attrs::new(0, 0, 0o644), None)
        .unwrap();
    let r = records::build(&RecordInput {
        tree: &tree,
        extents: &[],
        incompatible_features: 0x208,
        readonly_compatible_features: 0,
        unicode: UnicodeVersion::IOS_17,
        time_ns: 0,
    });
    assert!(matches!(r, Err(crate::WriteError::Name(_))));
}
