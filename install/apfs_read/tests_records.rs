// SPDX-License-Identifier: GPL-3.0-or-later
//! Records macOS 27.2's kernel wrote into a `newfs_apfs -e` volume (read_fixture_test.sh's plain fixture), decoded.

use std::cmp::Ordering;

use crate::catalog::{self, DirRec, FileExtent, Inode, Record, SiblingLink, Xattr, XattrData, xfields};

pub fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

fn inode(value: &str) -> Inode {
    Inode::decode(0, &unhex(value)).unwrap()
}

fn xattr(key: &str, value: &str) -> Xattr {
    Xattr::decode(&unhex(key), &unhex(value)).unwrap()
}

const D1_INODE: &str = "1300000000000000d50b0000000000007dc07296c67aeb18b5957396c67aeb1843d1d6a4c67aeb187dc07296c67aeb180080000000000000\
    03000000000000000200000000000000f501000014000000a48100000000000000000000020030000402030008202800643100000000000007000000\
    000000000010000000000000000000000000000007000000000000000000000000000000";

#[test]
fn hard_link_inode_siblings_and_drec() {
    let i = inode(D1_INODE);
    assert_eq!(
        (i.val.nchildren_or_nlink.get(), i.val.owner.get(), i.val.group.get()),
        (3, 501, 20)
    );
    assert_eq!(i.val.mode.get(), 0o100644);
    assert!(i.is_regular() && !i.is_compressed());
    assert_eq!(i.name().as_deref(), Some("d1"));
    assert_eq!(
        i.dstream().map(|d| (d.size.get(), d.alloced_size.get())),
        Some((7, 4096))
    );
    assert_eq!(
        i.xfields.iter().map(|x| (x.kind, x.flags)).collect::<Vec<_>>(),
        [(4, 2), (8, 0x20)]
    );

    let links: Vec<SiblingLink> = [
        ("d50b0000000000506c0c000000000000", "13000000000000000300643100"),
        ("d50b0000000000506d0c000000000000", "14000000000000000600686172643100"),
        ("d50b0000000000506e0c000000000000", "15000000000000000600686172643200"),
    ]
    .iter()
    .map(|(k, v)| SiblingLink::decode(&unhex(k), &unhex(v)).unwrap())
    .collect();
    let names: Vec<_> = links
        .iter()
        .map(|l| (l.sibling_id, l.parent_id, l.name.as_str()))
        .collect();
    assert_eq!(
        names,
        [(0xc6c, 0x13, "d1"), (0xc6d, 0x14, "hard1"), (0xc6e, 0x15, "hard2")]
    );

    let drec = DirRec::decode(
        &unhex("14000000000000900638ac88686172643100"),
        &unhex("d50b000000000000850b82a4c67aeb18080001000800010208006d0c000000000000"),
        true,
    )
    .unwrap();
    assert_eq!(
        (drec.parent, drec.name.as_str(), drec.file_id(), drec.dtype()),
        (0x14, "hard1", 0xbd5, 8)
    );
    assert_eq!(drec.sibling_id(), Some(0xc6d));
    assert_eq!(drec.hash, Some(0x88ac3806 >> 10));

    let map = Record::decode(&unhex("6c0c0000000000c0"), &unhex("d50b000000000000"), true).unwrap();
    assert!(matches!(
        map,
        Record::SiblingMap {
            oid: 0xc6c,
            file_id: 0xbd5
        }
    ));
}

#[test]
fn xattrs_embedded_and_streamed() {
    let big = xattr(
        "d80b0000000000401000636f6d2e6578616d706c652e62696700",
        "11003000710c000000000000e02e0000000000000030000000000000000000000000000000000000000000000000000000000000",
    );
    assert_eq!((big.name.as_str(), big.flags), ("com.example.big", 0x11));
    let XattrData::Stream { id, dstream } = big.data else {
        panic!("embedded")
    };
    assert_eq!(
        (id, dstream.dstream.size.get(), dstream.dstream.alloced_size.get()),
        (0xc71, 12000, 12288)
    );

    let acl = xattr(
        "dd0b0000000000401a00636f6d2e6170706c652e73797374656d2e536563757269747900",
        "02004400012cc16d00000000000000000000000000000000000000000000000000000000000000000000000100000000abcdefabcdefab\
         cdefabcdef0000000c0000000200000010",
    );
    assert_eq!(acl.name, "com.apple.system.Security");
    assert!(matches!(&acl.data, XattrData::Embedded(b) if b.len() == 68 && b[..4] == [0x01, 0x2c, 0xc1, 0x6d]));

    let link = xattr(
        "6f0c0000000000401500636f6d2e6170706c652e66732e73796d6c696e6b00",
        "06000b002e2e2f646174612f643200",
    );
    assert_eq!(link.flags, 0x6);
    assert!(matches!(&link.data, XattrData::Embedded(b) if b == b"../data/d2\0"));
}

#[test]
fn compressed_files() {
    let words = inode(
        "0200000000000000720c00000000000037537b55c67aeb1837537b55c67aeb180b4d33acc67aeb1822f86354c67aeb18004004000000000001\
         000000000000002900000020000000f501000014000000a4810000bd0d2600000000000100080004020600776f726473000000",
    );
    assert!(words.is_compressed() && words.dstream().is_none());
    assert_eq!(words.val.uncompressed_size.get(), 2_493_885);
    let decmpfs = xattr(
        "720c0000000000401200636f6d2e6170706c652e6465636d70667300",
        "0200100066706d6308000000bd0d260000000000",
    );
    assert!(matches!(&decmpfs.data, XattrData::Embedded(b) if b[..8] == *b"fpmc\x08\0\0\0"));
    let fork = xattr(
        "720c0000000000401700636f6d2e6170706c652e5265736f75726365466f726b00",
        "01003000730c000000000000c9590f000000000000600f00000000000000000000000000c9590f00000000000000000000000000",
    );
    assert!(matches!(fork.data, XattrData::Stream { id: 0xc73, dstream } if dstream.dstream.size.get() == 1_006_025));

    // ditto stores a small file's decmpfs as a stream, beside an empty resource fork.
    let small = xattr(
        "780c0000000000401200636f6d2e6170706c652e6465636d70667300",
        "010030007a0c000000000000bb0000000000000000100000000000000000000000000000bb000000000000000000000000000000",
    );
    assert!(matches!(small.data, XattrData::Stream { id: 0xc7a, dstream } if dstream.dstream.size.get() == 0xbb));
}

#[test]
fn sparse_file_extents_and_a_clone_with_its_own_stream() {
    let sparse = inode(
        "02000000000000007b0c000000000000a7e2dbadc67aeb1877f2dcadc67aeb1877f2dcadc67aeb18a7e2dbadc67aeb18008200000000000001\
         000000000000000300000000000000f501000014000000a481000000000000000000000300380004020700082028000d28080073706172736500\
         0000903e000000000000903e000000000000000000000000000010000000000000000000000000000000803e0000000000",
    );
    assert_eq!(sparse.dstream().map(|d| d.size.get()), Some(4_100_096));
    assert_eq!(
        sparse.xfield(13).map(|x| x.data.clone()),
        Some(0x3e_8000u64.to_le_bytes().to_vec())
    );
    let hole = FileExtent::decode(
        &unhex("7b0c0000000000800000000000000000"),
        &unhex("00803e000000000000000000000000000000000000000000"),
    )
    .unwrap();
    let data = FileExtent::decode(
        &unhex("7b0c00000000008000803e0000000000"),
        &unhex("001000000000000062040000000000000000000000000000"),
    )
    .unwrap();
    assert_eq!((hole.logical_addr, hole.len, hole.phys_block_num), (0, 0x3e_8000, 0));
    assert_eq!(
        (data.logical_addr, data.len, data.phys_block_num, data.blocks()),
        (0x3e_8000, 4096, 0x462, 1)
    );

    let clone = Inode::decode(
        0xc7d,
        &unhex(
            "02000000000000007e0c0000000000005fc1e2a3c67aeb186faf78aec67aeb186faf78aec67aeb185fc1e2a3c67aeb1800840000000000\
             0001000000000000000200000000000000f501000014000000a48100000000000000000000020038000820280004020e0000c012000000\
             000000c0120000000000000000000000000001c01200000000000000000000000000636c6f6e652d6368616e676564000000",
        ),
    )
    .unwrap();
    assert_eq!((clone.oid, clone.val.private_id.get()), (0xc7d, 0xc7e));
    assert_eq!(clone.name().as_deref(), Some("clone-changed"));
    assert_eq!(clone.xfields.iter().map(|x| x.kind).collect::<Vec<_>>(), [8, 4]);
}

#[test]
fn names_as_stored() {
    let cases = [
        ("020000000000009009fcb52e746162096e616d6500", "tab\tname"),
        ("02000000000000900a942ede636166c3a92d6e666300", "caf\u{e9}-nfc"),
        ("02000000000000900cd871e66e6169cc8876652d6e666400", "nai\u{308}ve-nfd"),
    ];
    for (key, name) in cases {
        let d = DirRec::decode(&unhex(key), &unhex("760c000000000000eacb99acc67aeb180800"), true).unwrap();
        assert_eq!(d.name, name);
    }
}

#[test]
fn wrong_lengths_are_refused() {
    let key = unhex("6f0c0000000000401500636f6d2e6170706c652e66732e73796d6c696e6b00");
    assert!(Xattr::decode(&key, &unhex("06000c002e2e2f646174612f643200")).is_err());
    assert!(Xattr::decode(&key[..key.len() - 1], &unhex("06000b002e2e2f646174612f643200")).is_err());
    assert!(DirRec::decode(&unhex("14000000000000900638ac886861726431"), &[0; 18], true).is_err());
    assert!(xfields(&unhex("0100090004020500726f6f7400000000")).is_err());
    assert!(xfields(&unhex("0100080004020900726f6f7400000000")).is_err());
    assert!(FileExtent::decode(&unhex("7b0c000000000080"), &[0; 24]).is_err());
}

#[test]
fn key_order_is_by_object_then_type() {
    let key = |oid: u64, kind: u8| (oid | (u64::from(kind) << 60)).to_le_bytes();
    assert_eq!(catalog::order(&key(2, 9), 3, Some(3)).unwrap(), Ordering::Less);
    assert_eq!(catalog::order(&key(3, 3), 3, Some(9)).unwrap(), Ordering::Less);
    assert_eq!(catalog::order(&key(3, 9), 3, None).unwrap(), Ordering::Equal);
    assert_eq!(catalog::order(&key(4, 1), 3, Some(13)).unwrap(), Ordering::Greater);
    assert!(catalog::order(&[1, 2, 3], 3, None).is_err());
}
