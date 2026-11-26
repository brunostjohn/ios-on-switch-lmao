// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::clone_mapping::{
    APFS_TYPE_CLONE_MAPPING, CLONE_MAPPING_FLAGS, CLONE_MAPPING_OID, CloneMappingKey, CloneMappingVal,
};
use switch_apfs_format::fs::{
    APFS_TYPE_DSTREAM_ID, DT_REG, Dstream, INODE_NO_RSRC_FORK, JKey, ROOT_DIR_INO_NUM, S_IFREG,
};
use switch_apfs_format::fs_records::{
    APFS_TYPE_DIR_STATS, APFS_TYPE_FILE_INFO, APFS_TYPE_SIBLING_MAP, DirStatsVal, FileDataHashVal, FileInfoKey,
    J_FILE_INFO_TYPE_SHIFT, RESOURCE_FORK_XATTR_NAME, XATTR_DATA_EMBEDDED,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_read::{Container, Fs, PhysExtent, Step, audit, stream};
use switch_apfs_write::extents::Owned;
use switch_apfs_write::inode::{INODE_HAS_RSRC_FORK, INODE_WAS_CLONED, INODE_WAS_EVER_CLONED};
use switch_apfs_write::target::MemTarget;
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U32, U64};

use crate::file_info::{encode_file_info, hash_reader};
use crate::tests_fixture::{Fixture, InodeMeta};
use crate::{Replacement, ReplacementData, SealError, SealOptions, plan, seal};

const REBUILD_TIME_NS: u64 = 1_795_000_000_000_000_000;

fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

fn read_bytes(img: &dyn BlockSource, path: &str) -> Vec<u8> {
    let c = Container::open(img).unwrap();
    let fs = Fs::open(img, &c.volumes[0]).unwrap();
    let oid = fs.lookup(path).unwrap().unwrap();
    let (entry, _) = fs.entry(oid, path.into(), None).unwrap();
    let mut out = Vec::new();
    if let Some(s) = entry.data {
        stream::read(img, &s, &mut |ch| out.extend_from_slice(ch)).unwrap();
    }
    out
}

#[test]
fn clone_owner_replaced_with_empty_allocates_fresh_private_id() {
    let mut b = Fixture::empty_root(2);
    let data = vec![0x5Au8; 20_000];
    let ds = b.add_stream_refcnt(16, 16, &data, Some(2));
    for (oid, name) in [(16u64, "clone_a"), (17, "clone_b")] {
        b.add_entry(
            oid,
            ROOT_DIR_INO_NUM,
            name,
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                private_id: Some(16),
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK | INODE_WAS_CLONED | INODE_WAS_EVER_CLONED,
                dstream: Some(ds),
                ..Default::default()
            },
        );
    }
    let src = b.finish();
    let opts = SealOptions::new([0x12; 16], REBUILD_TIME_NS);
    let mut out = MemTarget::default();
    seal(
        &src,
        &[Replacement {
            path: "/clone_a".into(),
            data: ReplacementData::Bytes(Arc::from(&[][..])),
            sha256_in: Some(sha256(&data)),
            sha256_out: Some(sha256(b"")),
        }],
        &opts,
        &mut out,
    )
    .unwrap();

    let c = Container::open(&out).unwrap();
    let aud = audit(&c, &out).unwrap();
    assert!(aud.ok(), "audit: {:?}", aud.problems);
    let fs = Fs::open(&out, &c.volumes[0]).unwrap();
    let ino_a = fs.inode(16).unwrap().unwrap();
    let ino_b = fs.inode(17).unwrap().unwrap();
    assert_ne!(ino_a.val.private_id.get(), 16);
    assert_eq!(ino_b.val.private_id.get(), 16);
    assert!(ino_a.dstream().is_none());
    assert!(ino_b.dstream().is_some());
    assert_eq!(fs.walk("/", &mut |_| Ok(Step::Into)).unwrap(), 3);
    assert_eq!(read_bytes(&out, "/clone_a"), b"");
    assert_eq!(read_bytes(&out, "/clone_b"), data);
}

#[test]
fn kept_zero_size_dstream_is_stripped_without_dangling_dstream_id() {
    let mut b = Fixture::empty_root(1);
    let ds = Dstream {
        size: U64::ZERO,
        alloced_size: U64::ZERO,
        total_bytes_written: U64::new(1),
        total_bytes_read: U64::new(1),
        ..Default::default()
    };
    b.push_dstream_id(16, 1);
    b.add_entry(
        16,
        ROOT_DIR_INO_NUM,
        "trunc_zero",
        DT_REG,
        S_IFREG | 0o644,
        InodeMeta {
            nlink: 1,
            iflags: INODE_NO_RSRC_FORK,
            dstream: Some(ds),
            ..Default::default()
        },
    );
    let src = b.finish();
    let mut out = MemTarget::default();
    seal(&src, &[], &SealOptions::new([0x14; 16], REBUILD_TIME_NS), &mut out).unwrap();

    let c = Container::open(&out).unwrap();
    let aud = audit(&c, &out).unwrap();
    assert!(aud.ok(), "audit: {:?}", aud.problems);
    let fs = Fs::open(&out, &c.volumes[0]).unwrap();
    let ino = fs.inode(16).unwrap().unwrap();
    assert!(ino.dstream().is_none());
    assert!(fs.object(16, Some(APFS_TYPE_DSTREAM_ID)).unwrap().is_empty());
    assert_eq!(fs.walk("/", &mut |_| Ok(Step::Into)).unwrap(), 2);
}

#[test]
fn unicode_nfc_and_nfd_lookups_match_each_other() {
    let mut b = Fixture::empty_root(2);
    let ds1 = b.add_stream_refcnt(16, 16, b"nfd file", Some(1));
    let ds2 = b.add_stream_refcnt(17, 17, b"nfc file", Some(1));
    for (oid, name, ds) in [(16u64, "cafe\u{0301}", ds1), (17, "na\u{00ef}ve", ds2)] {
        b.add_entry(
            oid,
            ROOT_DIR_INO_NUM,
            name,
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK,
                dstream: Some(ds),
                ..Default::default()
            },
        );
    }
    let src = b.finish();
    let mut out = MemTarget::default();
    seal(
        &src,
        &[
            Replacement {
                path: "/caf\u{00e9}".into(),
                data: ReplacementData::Bytes(Arc::from(&b"nfc replacement"[..])),
                sha256_in: Some(sha256(b"nfd file")),
                sha256_out: Some(sha256(b"nfc replacement")),
            },
            Replacement {
                path: "/nai\u{0308}ve".into(),
                data: ReplacementData::Bytes(Arc::from(&b"nfd replacement"[..])),
                sha256_in: Some(sha256(b"nfc file")),
                sha256_out: Some(sha256(b"nfd replacement")),
            },
        ],
        &SealOptions::new([0x17; 16], REBUILD_TIME_NS),
        &mut out,
    )
    .unwrap();

    assert_eq!(read_bytes(&out, "/cafe\u{0301}"), b"nfc replacement");
    assert_eq!(read_bytes(&out, "/na\u{00ef}ve"), b"nfd replacement");
}

#[test]
fn dropped_embedded_rsrc_fork_clears_inode_has_rsrc_fork() {
    let mut b = Fixture::empty_root(1);
    let ds = b.add_stream_refcnt(16, 16, b"plain data", Some(1));
    b.add_entry(
        16,
        ROOT_DIR_INO_NUM,
        "rsrc",
        DT_REG,
        S_IFREG | 0o644,
        InodeMeta {
            nlink: 1,
            iflags: INODE_HAS_RSRC_FORK,
            dstream: Some(ds),
            ..Default::default()
        },
    );
    b.add_xattr(16, RESOURCE_FORK_XATTR_NAME, XATTR_DATA_EMBEDDED, b"tiny fork");
    let src = b.finish();
    let mut out = MemTarget::default();
    seal(&src, &[], &SealOptions::new([0x18; 16], REBUILD_TIME_NS), &mut out).unwrap();

    let c = Container::open(&out).unwrap();
    let fs = Fs::open(&out, &c.volumes[0]).unwrap();
    let ino = fs.inode(16).unwrap().unwrap();
    assert!(fs.xattrs(16).unwrap().is_empty());
    assert_eq!(ino.val.internal_flags.get() & INODE_HAS_RSRC_FORK, 0);
    assert_ne!(ino.val.internal_flags.get() & INODE_NO_RSRC_FORK, 0);
}

#[test]
fn shared_physical_extents_deduplicate_with_exact_extent_refs() {
    let mut b = Fixture::empty_root(2);
    let shared_payload: Vec<u8> = (0..8 * BLOCK_SIZE).map(|i| (i % 251) as u8).collect();
    let (shared_paddr, _) = b.write_blocks_data(&shared_payload);
    let mut part_b_head = vec![0xEEu8; BLOCK_SIZE];
    part_b_head[..5].copy_from_slice(b"PATCH");
    let (head_paddr, _) = b.write_blocks_data(&part_b_head);

    b.push_fext(16, 0, 8 * BLOCK_SIZE as u64, 0, shared_paddr);
    b.owned.push(Owned {
        paddr: shared_paddr,
        blocks: 8,
        owner: 16,
    });
    b.push_dstream_id(16, 1);
    for c in &hash_reader(&mut Cursor::new(&shared_payload)).unwrap().chunks {
        b.fs.push(encode_file_info(16, c));
    }

    let mut part_b_bytes = part_b_head;
    part_b_bytes.extend_from_slice(&shared_payload[BLOCK_SIZE..]);
    b.push_fext(17, 0, BLOCK_SIZE as u64, 0, head_paddr);
    b.push_fext(17, BLOCK_SIZE as u64, 7 * BLOCK_SIZE as u64, 0, shared_paddr + 1);
    b.owned.push(Owned {
        paddr: head_paddr,
        blocks: 1,
        owner: 17,
    });
    b.owned.push(Owned {
        paddr: shared_paddr + 1,
        blocks: 7,
        owner: 17,
    });
    b.push_dstream_id(17, 1);
    for c in &hash_reader(&mut Cursor::new(&part_b_bytes)).unwrap().chunks {
        b.fs.push(encode_file_info(17, c));
    }

    for (oid, name) in [(16u64, "part_a"), (17, "part_b")] {
        b.add_entry(
            oid,
            ROOT_DIR_INO_NUM,
            name,
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK,
                dstream: Some(Dstream {
                    size: U64::new(8 * BLOCK_SIZE as u64),
                    alloced_size: U64::new(8 * BLOCK_SIZE as u64),
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    let src = b.finish();
    let mut out = MemTarget::default();
    let stats = seal(&src, &[], &SealOptions::new([0x22; 16], REBUILD_TIME_NS), &mut out).unwrap();
    assert_eq!(stats.data_blocks, 9, "shared 7 blocks must not be duplicated");

    let c = Container::open(&out).unwrap();
    let aud = audit(&c, &out).unwrap();
    assert!(aud.ok(), "audit: {:?}", aud.problems);
    assert_eq!(aud.volumes[0].owned, aud.volumes[0].fs_alloc_count + 1);
    assert_eq!(aud.volumes[0].shared_blocks, 7);

    let fs = Fs::open(&out, &c.volumes[0]).unwrap();
    let mut refs = Vec::new();
    fs.physical_tree(fs.sb.extentref_tree_oid.get(), &mut |k, v| {
        refs.push(PhysExtent::decode(k, v.unwrap())?);
        Ok(())
    })
    .unwrap();
    let counts: Vec<(u64, u64, i32)> = refs.iter().map(|p| (p.blocks, p.owner, p.refcnt)).collect();
    assert_eq!(counts, vec![(1, 16, 1), (7, 16, 2), (1, 17, 1)]);
    assert_eq!(read_bytes(&out, "/part_a"), shared_payload);
    assert_eq!(read_bytes(&out, "/part_b"), part_b_bytes);
}

#[test]
fn unsupported_and_malformed_source_records_are_refused() {
    let opts = SealOptions::new([0x23; 16], REBUILD_TIME_NS);

    {
        let mut b = Fixture::empty_root(0);
        let k = CloneMappingKey {
            hdr: JKey::new(CLONE_MAPPING_OID, APFS_TYPE_CLONE_MAPPING),
            flags: U32::new(CLONE_MAPPING_FLAGS),
            private_id: U64::new(16),
            file_id: U64::new(16),
        };
        b.fs.push((
            k.as_bytes().to_vec(),
            Some(
                CloneMappingVal {
                    physical_size: U64::new(4096),
                }
                .as_bytes()
                .to_vec(),
            ),
        ));
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }

    {
        let mut b = Fixture::empty_root(0);
        b.fs.push((
            JKey::new(ROOT_DIR_INO_NUM, APFS_TYPE_DIR_STATS).as_bytes().to_vec(),
            Some(DirStatsVal::default().as_bytes().to_vec()),
        ));
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }

    {
        let mut b = Fixture::empty_root(0);
        let k = FileInfoKey {
            hdr: JKey::new(16, APFS_TYPE_FILE_INFO),
            info_and_lba: U64::new(1u64 << J_FILE_INFO_TYPE_SHIFT),
        };
        let mut v = FileDataHashVal {
            hashed_len: U16::new(1),
            hash_size: 16,
        }
        .as_bytes()
        .to_vec();
        v.extend_from_slice(&[0u8; 16]);
        b.fs.push((k.as_bytes().to_vec(), Some(v)));
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }

    {
        let mut b = Fixture::empty_root(0);
        b.fs.push((
            JKey::new(30, APFS_TYPE_SIBLING_MAP).as_bytes().to_vec(),
            Some(vec![1, 2, 3]),
        ));
        assert!(matches!(plan(&b.finish(), &[], &opts), Err(SealError::Read(_))));
    }
}
