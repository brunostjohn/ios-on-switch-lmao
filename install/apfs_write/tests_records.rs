// SPDX-License-Identifier: GPL-3.0-or-later
//! Clones, hard links, extent references and placement, against the records macOS 27.2's kext wrote for the same
//! files on a Data volume with iOS's bits (out/agent-scratch/creator/apfs-w4/probe). Apple's inode numbers differ
//! from ours, so each expected record is Apple's bytes with our numbers put in.

use std::sync::Arc;

use switch_apfs_alloc::Bitmap;

use crate::content::{Content, SourceExtent};
use crate::extents::{self, Owned};
use crate::place::{FreeRuns, PlacedExtent, place};
use crate::tests::{build, dense, hex, le, value};
use crate::tree::{Attrs, Tree};

fn find(r: &crate::records::Records, key: &str) -> Option<Vec<u8>> {
    r.fs.iter()
        .find(|(k, _)| k == &hex(key))
        .map(|(_, v)| v.clone().unwrap())
}

#[test]
fn a_whole_file_clone_shares_its_stream_and_is_mapped() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let d = tree.add_dir(tree.root(), "d", Attrs::new(501, 20, 0o755)).unwrap();
    let s = tree.add_stream(0x200000, Content::Bytes(Arc::from(vec![0u8; 0x200000])));
    let x = tree.add_file(d, "x", Attrs::new(501, 20, 0o644), Some(s)).unwrap();
    let y = tree.add_file(d, "y", Attrs::new(501, 20, 0o644), Some(s)).unwrap();
    let (x, y) = (tree.number(x), tree.number(y));
    let r = build(&tree, &[dense(0x6de8, 512)], 0x208, 0xc);
    assert_eq!(value(&r, &format!("{}60", &le(x)[..14])), hex("02000000"));
    for file in [x, y] {
        let key = format!("09000000000000e013000000{}{}", le(x), le(file));
        assert_eq!(find(&r, &key), Some(hex("0000200000000000")), "clone mapping of {file}");
    }
    let y_inode = value(&r, &format!("{}30", &le(y)[..14]));
    assert_eq!(&y_inode[8..16], &x.to_le_bytes(), "y's private id is x's");
    assert_eq!(r.clone_mappings, 2);
    assert_eq!(r.data_blocks, 512);

    let r = build(&tree, &[dense(0x6de8, 512)], 0x208, 0x8);
    assert_eq!(r.clone_mappings, 0, "no mapping without rocompat 0x4");
}

#[test]
fn a_hard_link_has_siblings() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let d = tree.add_dir(tree.root(), "d", Attrs::new(501, 20, 0o755)).unwrap();
    let a = tree.add_dir(d, "a", Attrs::new(501, 20, 0o755)).unwrap();
    let data = tree.add_bytes(&b"hard\n"[..]);
    let h = tree.add_file(d, "h1", Attrs::new(501, 20, 0o644), Some(data)).unwrap();
    tree.add_link(h, d, "h2").unwrap();
    tree.add_link(h, a, "h3").unwrap();
    let (dn, an, hn) = (tree.number(d), tree.number(a), tree.number(h));
    let first = tree.next_number();
    let r = build(&tree, &[dense(0x168, 1)], 0x208, 0xc);
    let inode = value(&r, &format!("{}30", &le(hn)[..14]));
    assert_eq!(&inode[56..60], &3i32.to_le_bytes(), "three links");
    assert_eq!(&inode[0..8], &dn.to_le_bytes(), "the primary link's parent");
    let links = [(dn, "h1", "6831"), (dn, "h2", "6832"), (an, "h3", "6833")];
    for (n, (parent, _, name)) in links.iter().enumerate() {
        let sibling = first + n as u64;
        assert_eq!(
            find(&r, &format!("{}50{}", &le(hn)[..14], le(sibling))),
            Some(hex(&format!("{}0300{name}00", le(*parent)))),
            "sibling link {n}"
        );
        assert_eq!(
            find(&r, &format!("{}c0", &le(sibling)[..14])),
            Some(hn.to_le_bytes().to_vec())
        );
    }
    let drec =
        r.fs.iter()
            .find(|(k, _)| k.ends_with(b"h2\0") && k[..8] == hex(&format!("{}90", &le(dn)[..14]))[..])
            .unwrap()
            .1
            .clone()
            .unwrap();
    assert_eq!(&drec[16..], &hex(&format!("08000100080001020800{}", le(first + 1)))[..]);
    assert_eq!(r.next_obj_id, first + 3);
}

#[test]
fn shared_blocks_split_the_extent_references_as_macos_does() {
    let owned = [
        Owned {
            paddr: 0x6de8,
            blocks: 512,
            owner: 0x17,
        },
        Owned {
            paddr: 0x174,
            blocks: 4,
            owner: 0x19,
        },
        Owned {
            paddr: 0x6dec,
            blocks: 0x1fc,
            owner: 0x19,
        },
    ];
    let refs = extents::build(&owned);
    let apple = [
        ("7401000000000020", "0400000000000010190000000000000001000000"),
        ("e86d000000000020", "0400000000000010170000000000000001000000"),
        ("ec6d000000000020", "fc01000000000010170000000000000002000000"),
    ];
    let expected: Vec<_> = apple.iter().map(|(k, v)| (hex(k), Some(hex(v)))).collect();
    assert_eq!(refs.records, expected);
    assert_eq!((refs.blocks, refs.shared_blocks), (516, 0x1fc));
}

#[test]
fn keys_sort_as_macos_stores_them() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let f = tree
        .add_file(tree.root(), "words", Attrs::new(0, 0, 0o644), None)
        .unwrap();
    tree.set_xattr(f, "com.apple.decmpfs", b"fpmc");
    tree.set_xattr(f, "com.apple.ResourceFork", b"fork");
    let r = build(&tree, &[], 0x208, 0xc);
    let names: Vec<&[u8]> =
        r.fs.iter()
            .filter(|(k, _)| k[7] == 0x40)
            .map(|(k, _)| &k[10..])
            .collect();
    assert_eq!(names, [&b"com.apple.ResourceFork\0"[..], b"com.apple.decmpfs\0"]);

    let mut keys = vec![
        (
            hex(&format!("09000000000000e013000000{}{}", le(0x100), le(0x100))),
            None,
        ),
        (hex(&format!("09000000000000e013000000{}{}", le(0x17), le(0x200))), None),
        (hex(&format!("09000000000000e013000000{}{}", le(0x17), le(0x18))), None),
    ];
    crate::keys::sort(&mut keys);
    let order: Vec<u64> = keys
        .iter()
        .map(|(k, _)| u64::from_le_bytes(k[20..28].try_into().unwrap()))
        .collect();
    assert_eq!(order, [0x18, 0x200, 0x100]);
}

#[test]
fn source_extents_follow_their_relocation_pieces() {
    let mut used = Bitmap::new(40);
    for r in [0..10, 12..20, 30..40] {
        used.set_range(r);
    }
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let extents = vec![
        SourceExtent {
            logical: 0,
            len: 3 * 4096,
            flags: 0,
            paddr: 100,
            crypto_id: 0,
        },
        SourceExtent {
            logical: 3 * 4096,
            len: 4096,
            flags: 0,
            paddr: 0,
            crypto_id: 0,
        },
    ];
    let s = tree.add_stream(4 * 4096, Content::Source(Arc::from(extents)));
    tree.add_file(tree.root(), "f", Attrs::new(0, 0, 0o644), Some(s))
        .unwrap();
    let mut free = FreeRuns::from_bitmap(&used);
    let p = place(&[&tree], &mut free).unwrap();
    let pieces: Vec<_> = p.relocations.iter().map(|r| (r.src, r.dst, r.blocks)).collect();
    assert_eq!(pieces, [(100, 10, 2), (102, 20, 1)]);
    let placed: Vec<_> = p.extents[0][0].iter().map(|e| (e.logical, e.len, e.paddr)).collect();
    assert_eq!(placed, [(0, 8192, 10), (8192, 4096, 20), (12288, 4096, 0)]);
    assert_eq!(free.take_contiguous(9).unwrap(), 21);
    assert!(free.take_contiguous(1).is_err());
}

#[test]
fn a_fill_stream_gets_one_contiguous_run() {
    struct Zeros;
    impl crate::content::Fill for Zeros {
        fn fill(&self, _: &mut dyn switch_apfs_format::io::BlockTarget) -> std::io::Result<()> {
            Ok(())
        }
    }
    let mut used = Bitmap::new(100);
    for r in [0..10, 12..20] {
        used.set_range(r);
    }
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let small = tree.add_bytes(vec![1u8; 4096 * 3]);
    let big = tree.add_stream(4096 * 50, Content::Fill(Arc::new(Zeros)));
    tree.add_file(tree.root(), "small", Attrs::new(0, 0, 0o644), Some(small))
        .unwrap();
    tree.add_file(tree.root(), "big", Attrs::new(0, 0, 0o644), Some(big))
        .unwrap();
    let p = place(&[&tree], &mut FreeRuns::from_bitmap(&used)).unwrap();
    let small: Vec<PlacedExtent> = p.extents[0][0].clone();
    assert_eq!(
        small.iter().map(|e| (e.paddr, e.len)).collect::<Vec<_>>(),
        [(10, 8192), (20, 4096)]
    );
    assert_eq!(
        p.extents[0][1].iter().map(|e| (e.paddr, e.len)).collect::<Vec<_>>(),
        [(21, 50 * 4096)]
    );
}

#[test]
fn whole_file_clones_are_marked_cloned_on_both_sides() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let s = tree.add_bytes(vec![1u8; 4096]);
    let x = tree
        .add_file(tree.root(), "x", Attrs::new(0, 0, 0o644), Some(s))
        .unwrap();
    let y = tree
        .add_file(tree.root(), "y", Attrs::new(0, 0, 0o644), Some(s))
        .unwrap();
    let copied = tree.add_bytes(vec![2u8; 4096]);
    let stored = crate::tree::Stored {
        internal_flags: 0x8000,
        ..Default::default()
    };
    let p = tree.push(
        crate::tree::Kind::File,
        Attrs::new(0, 0, 0o644),
        Some(copied),
        Some(stored.clone()),
    );
    tree.link(p, tree.root(), "p", None, None, Vec::new()).unwrap();
    let q = tree.push(
        crate::tree::Kind::File,
        Attrs::new(0, 0, 0o644),
        Some(copied),
        Some(stored),
    );
    tree.link(q, tree.root(), "q", None, None, Vec::new()).unwrap();
    let lone = tree.add_bytes(vec![3u8; 4096]);
    let z = tree
        .add_file(tree.root(), "z", Attrs::new(0, 0, 0o644), Some(lone))
        .unwrap();
    let r = build(&tree, &[dense(100, 1), dense(101, 1), dense(102, 1)], 0x208, 0xc);
    let flags = |inode| {
        let key = format!("{}30", &le(tree.number(inode))[..14]);
        u64::from_le_bytes(value(&r, &key)[48..56].try_into().unwrap())
    };
    for (name, inode, want) in [
        ("x", x, 0x8410),
        ("y", y, 0x8410),
        ("p", p, 0x8410),
        ("q", q, 0x8410),
        ("z", z, 0x8000),
    ] {
        assert_eq!(flags(inode), want, "{name}'s internal flags");
    }
}

#[test]
fn copied_compressed_clones_keep_one_private_id_and_their_mappings() {
    use crate::tree::{Kind, Stored, Xattr, XattrValue};
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let copied = |tree: &mut Tree, name: &str, private: u64, xattr: &str| {
        let stream = tree.add_bytes(vec![7u8; 8192]);
        let stored = Stored {
            private_id: Some(private),
            internal_flags: 0x4_4410,
            ..Default::default()
        };
        let r = tree.push(Kind::File, Attrs::new(0, 0, 0o644), None, Some(stored));
        tree.link(r, tree.root(), name, None, None, Vec::new()).unwrap();
        tree.put_xattr(
            r,
            Xattr {
                name: xattr.into(),
                flags: 0,
                value: XattrValue::Stream(stream),
            },
        );
        tree.number(r)
    };
    let fork = "com.apple.ResourceFork";
    let words = copied(&mut tree, "words", 0x21, fork);
    let words_clone = copied(&mut tree, "words-clone", 0x21, fork);
    let small = copied(&mut tree, "small", 0x30, "com.apple.decmpfs");
    let small_clone = copied(&mut tree, "small-clone", 0x30, "com.apple.decmpfs");
    let lone = copied(&mut tree, "lone", 0x40, fork);
    let inode = |r: &crate::records::Records, n: u64| value(r, &format!("{}30", &le(n)[..14]));
    let private = |r: &crate::records::Records, n: u64| u64::from_le_bytes(inode(r, n)[8..16].try_into().unwrap());
    let mapping = |r: &crate::records::Records, id: u64, file: u64| {
        find(r, &format!("09000000000000e013000000{}{}", le(id), le(file)))
    };

    let extents = [
        dense(100, 2),
        dense(100, 2),
        dense(200, 1),
        dense(200, 1),
        dense(300, 2),
    ];
    let r = build(&tree, &extents, 0x208, 0xc);
    for (file, id) in [
        (words, words),
        (words_clone, words),
        (small, small),
        (small_clone, small),
        (lone, lone),
    ] {
        assert_eq!(private(&r, file), id, "private id of inode {file}");
    }
    for file in [words, words_clone] {
        assert_eq!(
            mapping(&r, words, file),
            Some(hex("0020000000000000")),
            "the fork's 8,192 bytes"
        );
    }
    for file in [small, small_clone] {
        assert_eq!(
            mapping(&r, small, file),
            Some(hex("0000000000000000")),
            "0 for a streamed decmpfs"
        );
    }
    assert_eq!(r.clone_mappings, 4);
    assert_eq!(r.shared_blocks, 3);

    let new = tree.add_bytes(&b"new\n"[..]);
    let replaced = tree.lookup("words-clone").unwrap();
    tree.replace_content(replaced, Some(new)).unwrap();
    let mut extents = extents.to_vec();
    extents.push(dense(400, 1));
    let r = build(&tree, &extents, 0x208, 0xc);
    assert_eq!(private(&r, words), words);
    assert_eq!(private(&r, words_clone), words_clone);
    assert_eq!(
        mapping(&r, words, words),
        None,
        "a clone whose other file was replaced is no longer mapped"
    );
    assert_eq!(r.clone_mappings, 2);
    let flags = u64::from_le_bytes(inode(&r, words_clone)[48..56].try_into().unwrap());
    assert_eq!(flags & 0x10, 0, "a replaced clone is no longer marked cloned");
}
