// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_alloc::Bitmap;
use switch_apfs_btree::Record;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{APFS_FILE_INFO_DATA_HASH, DECMPFS_XATTR_NAME, SYMLINK_XATTR_NAME, UF_COMPRESSED};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_read::{Container, FileInfo, Fs, XattrData, audit};
use switch_apfs_write::Fill;
use switch_apfs_write::inode::{INODE_HAS_SECURITY_EA, SECURITY_XATTR_NAME};
use switch_apfs_write::target::MemTarget;

use crate::file_info::{FILE_INFO_CHUNK_SIZE, encode_file_info, hash_reader};
use crate::records::{fs_key_order, keep_embedded_xattr, sort_fs_records};
use crate::tests_fixture::synthetic_sealed_source;
use crate::{Replacement, ReplacementData, SealError, SealOptions, load_manifest, plan, seal, seal_fill};

fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

fn padded_sha256(chunk: &[u8], blocks: usize) -> [u8; 32] {
    let mut buf = vec![0u8; blocks * BLOCK_SIZE];
    buf[..chunk.len()].copy_from_slice(chunk);
    sha256(&buf)
}

#[test]
fn file_info_chunk_hashing_and_padding() {
    let empty = hash_reader(&mut Cursor::new(&[][..])).unwrap();
    assert_eq!(empty.size, 0);
    assert_eq!(empty.sha256, sha256(b""));
    assert!(empty.chunks.is_empty());

    let one = hash_reader(&mut Cursor::new(&[0x42u8][..])).unwrap();
    assert_eq!(one.size, 1);
    assert_eq!(one.chunks.len(), 1);
    assert_eq!(one.chunks[0].offset, 0);
    assert_eq!(one.chunks[0].hashed_len, 1);
    assert_eq!(one.chunks[0].hash, padded_sha256(&[0x42], 1));

    let exact_block = vec![0xABu8; BLOCK_SIZE];
    let r = hash_reader(&mut Cursor::new(&exact_block)).unwrap();
    assert_eq!(r.chunks.len(), 1);
    assert_eq!(r.chunks[0].hashed_len, 1);
    assert_eq!(r.chunks[0].hash, sha256(&exact_block));

    let over_block = vec![0xCDu8; BLOCK_SIZE + 1];
    let r = hash_reader(&mut Cursor::new(&over_block)).unwrap();
    assert_eq!(r.chunks.len(), 1);
    assert_eq!(r.chunks[0].hashed_len, 2);
    assert_eq!(r.chunks[0].hash, padded_sha256(&over_block, 2));

    let exact_chunk = vec![0x11u8; FILE_INFO_CHUNK_SIZE];
    let r = hash_reader(&mut Cursor::new(&exact_chunk)).unwrap();
    assert_eq!(r.chunks.len(), 1);
    assert_eq!(r.chunks[0].hashed_len, 4);
    assert_eq!(r.chunks[0].hash, sha256(&exact_chunk));

    let multi: Vec<u8> = (0..(3 * FILE_INFO_CHUNK_SIZE + 5000))
        .map(|i| (i % 251) as u8)
        .collect();
    let r = hash_reader(&mut Cursor::new(&multi)).unwrap();
    assert_eq!(r.size, multi.len() as u64);
    assert_eq!(r.sha256, sha256(&multi));
    assert_eq!(r.chunks.len(), 4);
    for (idx, chunk) in r.chunks.iter().enumerate() {
        assert_eq!(chunk.offset, (idx * FILE_INFO_CHUNK_SIZE) as u64);
        let slice = &multi[idx * FILE_INFO_CHUNK_SIZE..((idx + 1) * FILE_INFO_CHUNK_SIZE).min(multi.len())];
        let want_blocks = slice.len().div_ceil(BLOCK_SIZE);
        assert_eq!(chunk.hashed_len, want_blocks as u16);
        assert_eq!(chunk.hash, padded_sha256(slice, want_blocks));
    }

    let (k, v) = encode_file_info(99, &r.chunks[3]);
    let decoded = FileInfo::decode(&k, v.as_deref().unwrap()).unwrap();
    assert_eq!(decoded.stream, 99);
    assert_eq!(decoded.info_type, APFS_FILE_INFO_DATA_HASH);
    assert_eq!(decoded.offset, (3 * FILE_INFO_CHUNK_SIZE) as u64);
    assert_eq!(decoded.hashed_len, 2);
    assert_eq!(decoded.hash, r.chunks[3].hash);
}

#[test]
fn file_info_keys_sort_numerically_past_64kib() {
    let offsets = [131_072u64, 0, 65_536, 16_384, 49_152, 32_768];
    let mut recs: Vec<Record> = offsets
        .iter()
        .map(|&off| {
            encode_file_info(
                42,
                &crate::ChunkHash {
                    offset: off,
                    hashed_len: 4,
                    hash: [0u8; 32],
                },
            )
        })
        .collect();
    sort_fs_records(&mut recs);
    let got: Vec<u64> = recs
        .iter()
        .map(|(k, v)| FileInfo::decode(k, v.as_deref().unwrap()).unwrap().offset)
        .collect();
    assert_eq!(got, vec![0, 16_384, 32_768, 49_152, 65_536, 131_072]);
    assert!(fs_key_order(&recs[2].0, &recs[4].0).is_lt());
}

#[test]
fn embedded_xattr_filter_rule() {
    assert!(keep_embedded_xattr(SYMLINK_XATTR_NAME));
    assert!(keep_embedded_xattr(DECMPFS_XATTR_NAME));
    assert!(!keep_embedded_xattr("com.apple.provenance"));
    assert!(!keep_embedded_xattr(SECURITY_XATTR_NAME));
    assert!(!keep_embedded_xattr("com.example.user"));
}

#[test]
fn rebuild_filters_xattrs_replaces_files_and_audits_clean() {
    let src = synthetic_sealed_source();
    let new_plain = vec![0x77u8; 20_000];
    let new_hl = b"replaced hardlinked bytes".to_vec();
    let new_comp = b"now uncompressed!".to_vec();
    let uuid = [0x42u8; 16];
    let time_ns = 1_795_000_000_000_000_000u64;

    let replacements = vec![
        Replacement {
            path: "/plain".into(),
            data: ReplacementData::Bytes(Arc::from(new_plain.as_slice())),
            sha256_in: Some(sha256(b"hello original plain file")),
            sha256_out: Some(sha256(&new_plain)),
        },
        Replacement {
            path: "/hl1".into(),
            data: ReplacementData::Bytes(Arc::from(new_hl.as_slice())),
            sha256_in: Some(sha256(b"hardlinked content")),
            sha256_out: Some(sha256(&new_hl)),
        },
        Replacement {
            path: "/hl2".into(),
            data: ReplacementData::Bytes(Arc::from(new_hl.as_slice())),
            sha256_in: Some(sha256(b"hardlinked content")),
            sha256_out: Some(sha256(&new_hl)),
        },
        Replacement {
            path: "/comp_type9".into(),
            data: ReplacementData::Bytes(Arc::from(new_comp.as_slice())),
            sha256_in: Some(sha256(b"inline compressed plaintext!")),
            sha256_out: Some(sha256(&new_comp)),
        },
    ];

    let mut opts = SealOptions::new(uuid, time_ns);
    opts.volume_name = Some("Rebuilt".into());

    let mut out1 = MemTarget::default();
    let stats1 = seal(&src, &replacements, &opts, &mut out1).unwrap();
    let mut out2 = MemTarget::default();
    let stats2 = seal(&src, &replacements, &opts, &mut out2).unwrap();
    assert_eq!(stats1.root_hash, stats2.root_hash);
    assert!(out1 == out2, "two builds must be byte-identical");
    assert_eq!(stats1.replaced_inodes, 3);

    let c = Container::open(&out1).unwrap();
    assert_eq!(c.volumes.len(), 1);
    assert_eq!(c.volumes[0].sb.vol_uuid, uuid);
    assert_eq!(c.volumes[0].sb.name(), "Rebuilt");
    let aud = audit(&c, &out1).unwrap();
    assert!(aud.ok(), "audit problems: {:?}", aud.problems);
    assert_eq!(aud.volumes[0].owned, aud.volumes[0].fs_alloc_count + 1);

    let fs = Fs::open(&out1, &c.volumes[0]).unwrap();
    assert_eq!(fs.seal.unwrap().root_hash, stats1.root_hash);

    let xino = fs.inode(18).unwrap().unwrap();
    assert_eq!(xino.val.internal_flags.get() & INODE_HAS_SECURITY_EA, 0);
    assert_eq!(xino.val.mod_time.get(), time_ns);
    let xs = fs.xattrs(18).unwrap();
    assert_eq!(xs.len(), 1);
    assert_eq!(xs[0].name, "com.example.big");
    assert_eq!(xs[0].flags & 0x10, 0);
    assert!(matches!(xs[0].data, XattrData::Stream { .. }));

    let sym_xs = fs.xattrs(19).unwrap();
    assert_eq!(sym_xs.len(), 1);
    assert_eq!(sym_xs[0].name, SYMLINK_XATTR_NAME);

    let cino = fs.inode(21).unwrap().unwrap();
    assert_eq!(cino.val.bsd_flags.get() & UF_COMPRESSED, 0);
    assert_eq!(cino.val.uncompressed_size.get(), 0);
    assert!(fs.xattrs(21).unwrap().is_empty());
    assert_eq!(fs.file_info(cino.val.private_id.get()).unwrap().len(), 1);

    let fino = fs.inode(22).unwrap().unwrap();
    assert_ne!(fino.val.bsd_flags.get() & UF_COMPRESSED, 0);
    assert_eq!(fs.xattrs(22).unwrap().len(), 2);

    assert_eq!(fs.lookup("/hl1").unwrap(), Some(17));
    assert_eq!(fs.lookup("/hl2").unwrap(), Some(17));
    let hino = fs.inode(17).unwrap().unwrap();
    assert_eq!(hino.val.nchildren_or_nlink.get(), 2);
    assert_eq!(hino.dstream().unwrap().size.get(), new_hl.len() as u64);

    let pino = fs.inode(16).unwrap().unwrap();
    let pinfo = fs.file_info(pino.val.private_id.get()).unwrap();
    assert_eq!(pinfo.len(), 2);
    assert_eq!((pinfo[0].offset, pinfo[0].hashed_len), (0, 4));
    assert_eq!((pinfo[1].offset, pinfo[1].hashed_len), (16_384, 1));
}

#[test]
fn seal_fill_writes_every_block_and_matches_seal() {
    let src = Arc::new(synthetic_sealed_source());
    let opts = SealOptions::new([0x99u8; 16], 1_795_000_000_000_000_000);
    let reps = vec![Replacement {
        path: "/plain".into(),
        data: ReplacementData::Bytes(Arc::from(&b"fill payload"[..])),
        sha256_in: None,
        sha256_out: None,
    }];

    let fill = seal_fill(src.clone(), &reps, &opts).unwrap();
    let blocks = fill.exact_blocks();
    assert_eq!(fill.exact_size(), blocks * BLOCK_SIZE as u64);

    struct TrackingTarget {
        mem: MemTarget,
        written: Bitmap,
    }
    impl BlockTarget for TrackingTarget {
        fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> std::io::Result<()> {
            let n = (data.len() / BLOCK_SIZE) as u64;
            assert!(paddr + n <= self.written.len());
            self.written.set_range(paddr..paddr + n);
            self.mem.write_blocks(paddr, data)
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    let mut tracked = TrackingTarget {
        mem: MemTarget::default(),
        written: Bitmap::new(blocks),
    };
    fill.fill(&mut tracked).unwrap();
    assert_eq!(
        tracked.written.count_set(0..blocks),
        blocks,
        "every block must be written"
    );

    let mut direct = MemTarget::default();
    seal(src.as_ref(), &reps, &opts, &mut direct).unwrap();
    for b in 0..blocks {
        let mut a = [0u8; BLOCK_SIZE];
        let mut c = [0u8; BLOCK_SIZE];
        tracked.mem.read_blocks(b, &mut a).unwrap();
        direct.read_blocks(b, &mut c).unwrap();
        assert_eq!(a, c, "block {b} differs between SealFill and seal()");
    }
}

#[test]
fn refusals_cover_every_contract() {
    let src = synthetic_sealed_source();
    let opts = SealOptions::new([0x33u8; 16], 1_795_000_000_000_000_000);
    let data = ReplacementData::Bytes(Arc::from(&b"abc"[..]));

    for bad in ["", "plain", "/", "/a/../plain", "/plain/"] {
        let err = plan(
            &src,
            &[Replacement {
                path: bad.into(),
                data: data.clone(),
                sha256_in: None,
                sha256_out: None,
            }],
            &opts,
        )
        .err()
        .unwrap();
        assert!(matches!(err, SealError::InvalidPath(_)), "{bad}: {err}");
    }

    let err = plan(
        &src,
        &[Replacement {
            path: "/does-not-exist".into(),
            data: data.clone(),
            sha256_in: None,
            sha256_out: None,
        }],
        &opts,
    )
    .err()
    .unwrap();
    assert!(matches!(err, SealError::MissingPath(_)), "{err}");

    for path in ["/dir", "/sym"] {
        let err = plan(
            &src,
            &[Replacement {
                path: path.into(),
                data: data.clone(),
                sha256_in: None,
                sha256_out: None,
            }],
            &opts,
        )
        .err()
        .unwrap();
        assert!(matches!(err, SealError::NotRegularFile(_)), "{path}: {err}");
    }

    let err = plan(
        &src,
        &[Replacement {
            path: "/with_xattrs".into(),
            data: data.clone(),
            sha256_in: None,
            sha256_out: None,
        }],
        &opts,
    )
    .err()
    .unwrap();
    assert!(matches!(err, SealError::ReplacedHasXattrs { .. }), "{err}");

    for with_hashes in [true, false] {
        let a = b"variant-a";
        let b = b"variant-b";
        let err = plan(
            &src,
            &[
                Replacement {
                    path: "/hl1".into(),
                    data: ReplacementData::Bytes(Arc::from(&a[..])),
                    sha256_in: None,
                    sha256_out: with_hashes.then(|| sha256(a)),
                },
                Replacement {
                    path: "/hl2".into(),
                    data: ReplacementData::Bytes(Arc::from(&b[..])),
                    sha256_in: None,
                    sha256_out: with_hashes.then(|| sha256(b)),
                },
            ],
            &opts,
        )
        .err()
        .unwrap();
        assert!(matches!(err, SealError::ConflictingHardLink { .. }), "{err}");
    }

    let err = plan(
        &src,
        &[Replacement {
            path: "/plain".into(),
            data: data.clone(),
            sha256_in: Some([0xFFu8; 32]),
            sha256_out: None,
        }],
        &opts,
    )
    .err()
    .unwrap();
    assert!(matches!(err, SealError::Sha256InMismatch { .. }), "{err}");

    let err = plan(
        &src,
        &[Replacement {
            path: "/plain".into(),
            data,
            sha256_in: None,
            sha256_out: Some([0xEEu8; 32]),
        }],
        &opts,
    )
    .err()
    .unwrap();
    assert!(matches!(err, SealError::Sha256OutMismatch { .. }), "{err}");
}

#[test]
fn replace_with_empty_drops_data_stream() {
    let src = synthetic_sealed_source();
    let opts = SealOptions::new([0x55u8; 16], 1_795_000_000_000_000_000);
    let reps = vec![Replacement {
        path: "/plain".into(),
        data: ReplacementData::Bytes(Arc::from(&[][..])),
        sha256_in: Some(sha256(b"hello original plain file")),
        sha256_out: Some(sha256(b"")),
    }];
    let mut out = MemTarget::default();
    seal(&src, &reps, &opts, &mut out).unwrap();
    let c = Container::open(&out).unwrap();
    let aud = audit(&c, &out).unwrap();
    assert!(aud.ok(), "audit problems: {:?}", aud.problems);
    let fs = Fs::open(&out, &c.volumes[0]).unwrap();
    let pino = fs.inode(16).unwrap().unwrap();
    assert!(pino.dstream().is_none());
    assert!(fs.file_info(16).unwrap().is_empty());
    assert!(fs.extents(16).unwrap().is_empty());
}

#[test]
fn manifest_json_round_trip() {
    let dir = tempfile::tempdir().unwrap();
    let file_path = dir.path().join("files/plain");
    std::fs::create_dir_all(file_path.parent().unwrap()).unwrap();
    std::fs::write(&file_path, b"new bytes").unwrap();
    let manifest_path = dir.path().join("app_cryptex.manifest.json");
    let h_in = hex::encode(sha256(b"old bytes"));
    let h_out = hex::encode(sha256(b"new bytes"));
    let json = format!(
        r#"{{"role":"app_cryptex","translator":"abc","files":[{{"path":"/plain","file":"files/plain","sha256_in":"{h_in}","sha256_out":"{h_out}","cdhash":[]}}]}}"#
    );
    std::fs::write(&manifest_path, json).unwrap();
    let reps = load_manifest(&manifest_path).unwrap();
    assert_eq!(reps.len(), 1);
    assert_eq!(reps[0].path, "/plain");
    assert_eq!(reps[0].sha256_in, Some(sha256(b"old bytes")));
    assert_eq!(reps[0].sha256_out, Some(sha256(b"new bytes")));
}
