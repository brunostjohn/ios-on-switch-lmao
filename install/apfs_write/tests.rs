// SPDX-License-Identifier: GPL-3.0-or-later
//! Records compared with the ones macOS 27.2's kext wrote for the same files on a Data-role volume with the spec's
//! feature bits. Times and the write-generation counter differ by nature.

use switch_apfs_names::UnicodeVersion;

use crate::records::{self, RecordInput};
use crate::spec::{Content, Entry, Owner, Tree};

fn hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

fn hello_tree() -> Tree {
    Tree {
        root: Owner {
            uid: 0,
            gid: 0,
            mode: 0o755,
        },
        entries: vec![Entry {
            name: "hello".into(),
            owner: Owner {
                uid: 0,
                gid: 0,
                mode: 0o644,
            },
            content: Content::File(b"hello\n".to_vec()),
        }],
    }
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
    let numbered = records::number(&tree);
    let (fs, extent_refs) = records::build(&RecordInput {
        tree: &tree,
        numbered: &numbered,
        extent_addrs: &[0xcb],
        incompatible_features: 0x208,
        unicode: UnicodeVersion::IOS_27,
        time_ns: 0,
    })
    .unwrap();
    let keys: Vec<String> = fs
        .iter()
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

    let hello = fs
        .iter()
        .find(|(k, _)| k == &hex("1000000000000030"))
        .unwrap()
        .1
        .clone()
        .unwrap();
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
    let mut ours = mask_inode(&hello);
    ours[8..16].copy_from_slice(&0x12u64.to_le_bytes());
    assert_eq!(ours, apple);

    let private = fs
        .iter()
        .find(|(k, _)| k == &hex("0300000000000030"))
        .unwrap()
        .1
        .clone()
        .unwrap();
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
    assert_eq!(mask_inode(&private), apple_private);

    let root = fs
        .iter()
        .find(|(k, _)| k == &hex("0200000000000030"))
        .unwrap()
        .1
        .clone()
        .unwrap();
    assert_eq!(&root[56..60], &1i32.to_le_bytes(), "one child");
    assert_eq!(&root[84..92], &0u64.to_le_bytes(), "no subdirectories");
    assert_eq!(&root[92..], &hex("0100080004020500726f6f7400000000")[..]);

    let extent = fs
        .iter()
        .find(|(k, _)| k == &hex("10000000000000800000000000000000"))
        .unwrap()
        .1
        .clone()
        .unwrap();
    assert_eq!(extent, hex("0010000000000000cb000000000000000000000000000000"));
    assert_eq!(
        extent_refs,
        vec![(
            hex("cb00000000000020"),
            Some(
                hex("01000000000000101000000000000000")
                    .into_iter()
                    .chain([1, 0, 0, 0])
                    .collect()
            )
        )]
    );
}

#[test]
fn subdirectory_count_follows_the_feature() {
    let mut tree = hello_tree();
    tree.entries.push(Entry {
        name: "sub".into(),
        owner: Owner {
            uid: 0,
            gid: 0,
            mode: 0o755,
        },
        content: Content::Dir(vec![]),
    });
    let numbered = records::number(&tree);
    for (incompat, expected) in [(0x208u64, 1u64), (0x8, 0)] {
        let (fs, _) = records::build(&RecordInput {
            tree: &tree,
            numbered: &numbered,
            extent_addrs: &[0xcb],
            incompatible_features: incompat,
            unicode: UnicodeVersion::IOS_27,
            time_ns: 0,
        })
        .unwrap();
        let root = &fs
            .iter()
            .find(|(k, _)| k == &hex("0200000000000030"))
            .unwrap()
            .1
            .as_ref()
            .unwrap()[..];
        assert_eq!(u64::from_le_bytes(root[84..92].try_into().unwrap()), expected);
        assert_eq!(i32::from_le_bytes(root[56..60].try_into().unwrap()), 2);
    }
}
