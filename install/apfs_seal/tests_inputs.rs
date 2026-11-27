// SPDX-License-Identifier: GPL-3.0-or-later
use std::cmp::Ordering;
use std::io::{self, Cursor, Read};
use std::path::Path;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_XATTR, DT_REG, Dstream, INODE_NO_RSRC_FORK, JKey, ROOT_DIR_INO_NUM, S_IFREG,
};
use switch_apfs_format::fs_records::{APFS_TYPE_FILE_INFO, RESOURCE_FORK_XATTR_NAME, XATTR_DATA_STREAM, XattrDstream};
use switch_apfs_format::io::BlockSource;
use switch_apfs_read::{Container, Fs, PhysExtent};
use switch_apfs_write::extents::Owned;
use switch_apfs_write::inode::INODE_HAS_RSRC_FORK;
use switch_apfs_write::target::MemTarget;
use zerocopy::IntoBytes;
use zerocopy::little_endian::U64;

use crate::delta::parse_replace;
use crate::file_info::{encode_file_info, hash_reader};
use crate::records::fs_key_order;
use crate::tests_fixture::{Fixture, InodeMeta, synthetic_sealed_source};
use crate::{Replacement, ReplacementData, SealError, SealOptions, load_manifest, plan, seal, seal_to_file};

const REBUILD_TIME_NS: u64 = 1_795_000_000_000_000_000;

fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

#[test]
fn parse_replace_preserves_windows_drive_letters_and_colons() {
    let h1 = "aa".repeat(32);
    let h2 = "bb".repeat(32);

    let r = parse_replace(r"/bin/app=C:\Users\test\app.bin").unwrap();
    assert_eq!(r.path, "/bin/app");
    assert!(matches!(r.data, ReplacementData::File(p) if p == Path::new(r"C:\Users\test\app.bin")));
    assert_eq!((r.sha256_in, r.sha256_out), (None, None));

    let r = parse_replace(&format!(r"/bin/app=C:\Users\test\app.bin:{h1}")).unwrap();
    assert!(matches!(r.data, ReplacementData::File(p) if p == Path::new(r"C:\Users\test\app.bin")));
    assert_eq!((r.sha256_in, r.sha256_out), (Some([0xAA; 32]), None));

    let r = parse_replace(&format!(r"/bin/app=C:\Users\test\app.bin:{h1}:{h2}")).unwrap();
    assert!(matches!(r.data, ReplacementData::File(p) if p == Path::new(r"C:\Users\test\app.bin")));
    assert_eq!((r.sha256_in, r.sha256_out), (Some([0xAA; 32]), Some([0xBB; 32])));

    let r = parse_replace(&format!(r"/bin/app=C:\Users\test\app.bin::{h2}")).unwrap();
    assert!(matches!(r.data, ReplacementData::File(p) if p == Path::new(r"C:\Users\test\app.bin")));
    assert_eq!((r.sha256_in, r.sha256_out), (None, Some([0xBB; 32])));

    let r = parse_replace(&format!("/bin/app=/tmp/dir:1/app:2.bin:{h1}:")).unwrap();
    assert!(matches!(r.data, ReplacementData::File(p) if p == Path::new("/tmp/dir:1/app:2.bin")));
    assert_eq!((r.sha256_in, r.sha256_out), (Some([0xAA; 32]), None));

    assert!(parse_replace(&format!("/bin/app=:{h1}")).is_err());
    assert!(parse_replace(&format!("/bin/app=::{h2}")).is_err());
    assert!(parse_replace("/bin/app=").is_err());
}

#[test]
fn fext_oob_overflow_and_trailing_holes_fail_cleanly_in_plan() {
    let opts = SealOptions::new([0x31; 16], REBUILD_TIME_NS);

    {
        let mut b = Fixture::empty_root(1);
        b.push_fext(16, 0, BLOCK_SIZE as u64, 0, 999_999);
        b.push_dstream_id(16, 1);
        b.add_entry(
            16,
            ROOT_DIR_INO_NUM,
            "oob",
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK,
                dstream: Some(Dstream {
                    size: U64::new(100),
                    alloced_size: U64::new(BLOCK_SIZE as u64),
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }

    {
        let mut b = Fixture::empty_root(1);
        let big_len = 0x00FF_FFFF_FFFF_F000u64;
        let mut at = 0u64;
        for i in 0..257u64 {
            b.push_fext(16, at, big_len, 0, 500 + i);
            at = at.wrapping_add(big_len);
            if at < big_len {
                break;
            }
        }
        b.push_dstream_id(16, 1);
        b.add_entry(
            16,
            ROOT_DIR_INO_NUM,
            "huge",
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK,
                dstream: Some(Dstream {
                    size: U64::new(4096),
                    alloced_size: U64::new(4096),
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }

    {
        let mut b = Fixture::empty_root(1);
        let (paddr, _) = b.write_blocks_data(&[0xAA; BLOCK_SIZE]);
        b.push_fext(16, 0, BLOCK_SIZE as u64, 0, paddr);
        b.owned.push(Owned {
            paddr,
            blocks: 1,
            owner: 16,
        });
        b.push_dstream_id(16, 1);
        let h = hash_reader(&mut Cursor::new(&vec![0xAA; BLOCK_SIZE])).unwrap();
        b.fs.push(encode_file_info(16, &h.chunks[0]));
        b.add_entry(
            16,
            ROOT_DIR_INO_NUM,
            "trail",
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_NO_RSRC_FORK,
                dstream: Some(Dstream {
                    size: U64::new(16_384),
                    alloced_size: U64::new(4096),
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
        assert!(matches!(
            plan(&b.finish(), &[], &opts),
            Err(SealError::InvalidSource(_))
        ));
    }
}

#[test]
fn load_manifest_rejects_host_path_traversal() {
    let dir = tempfile::tempdir().unwrap();
    for bad in ["/etc/hosts", "../../../etc/hosts", "files/../../etc/hosts", "", "."] {
        let p = dir.path().join("bad.manifest.json");
        std::fs::write(&p, format!(r#"{{"files":[{{"path":"/a","file":"{bad}"}}]}}"#)).unwrap();
        assert!(matches!(load_manifest(&p), Err(SealError::InvalidSpec(_))), "{bad}");
    }
    let ok = dir.path().join("ok.manifest.json");
    std::fs::write(&ok, r#"{"files":[{"path":"/a","file":"./files/a"}]}"#).unwrap();
    let reps = load_manifest(&ok).unwrap();
    assert!(matches!(&reps[0].data, ReplacementData::File(p) if p == &dir.path().join("files/a")));
}

#[test]
fn fs_key_order_is_bounds_safe_and_shared_with_apfs_write() {
    for short in [
        Vec::new(),
        vec![1, 2],
        JKey::new(2, APFS_TYPE_DIR_REC).as_bytes().to_vec(),
        JKey::new(2, APFS_TYPE_XATTR).as_bytes().to_vec(),
        JKey::new(2, APFS_TYPE_FILE_INFO).as_bytes().to_vec(),
    ] {
        assert_eq!(fs_key_order(&short, &short), Ordering::Equal);
    }
}

#[test]
fn options_validation_duplicate_path_eintr_xattr_owner_and_unlink_on_error() {
    let src = synthetic_sealed_source();

    for bad_name in [Some(String::new()), Some("Bad\0Name".into())] {
        let mut opts = SealOptions::new([0x30; 16], REBUILD_TIME_NS);
        opts.volume_name = bad_name;
        assert!(matches!(plan(&src, &[], &opts), Err(SealError::InvalidSpec(_))));
    }
    let mut opts = SealOptions::new([0x30; 16], REBUILD_TIME_NS);
    opts.formatted_by = "bad\0id".into();
    assert!(matches!(plan(&src, &[], &opts), Err(SealError::InvalidSpec(_))));

    let opts = SealOptions::new([0x33; 16], REBUILD_TIME_NS);
    let err = plan(
        &src,
        &[
            Replacement {
                path: "/plain".into(),
                data: ReplacementData::Bytes(Arc::from(&b"one"[..])),
                sha256_in: None,
                sha256_out: None,
            },
            Replacement {
                path: "/plain".into(),
                data: ReplacementData::Bytes(Arc::from(&b"two"[..])),
                sha256_in: None,
                sha256_out: None,
            },
        ],
        &opts,
    )
    .err()
    .unwrap();
    assert!(matches!(err, SealError::DuplicatePath(ref p) if p == "/plain"), "{err}");

    struct InterruptingReader {
        data: &'static [u8],
        pos: usize,
        interrupted: bool,
    }
    impl Read for InterruptingReader {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if !self.interrupted {
                self.interrupted = true;
                return Err(io::Error::from(io::ErrorKind::Interrupted));
            }
            let n = (self.data.len() - self.pos).min(buf.len());
            buf[..n].copy_from_slice(&self.data[self.pos..self.pos + n]);
            self.pos += n;
            Ok(n)
        }
    }
    let mut r = InterruptingReader {
        data: b"interrupted payload",
        pos: 0,
        interrupted: false,
    };
    assert_eq!(hash_reader(&mut r).unwrap().sha256, sha256(b"interrupted payload"));

    let mut b = Fixture::empty_root(2);
    let xds = XattrDstream {
        xattr_obj_id: U64::new(40),
        dstream: b.add_stream_refcnt(40, 16, b"shared fork", None),
    };
    for (oid, name) in [(16u64, "a"), (17, "b")] {
        b.add_entry(
            oid,
            ROOT_DIR_INO_NUM,
            name,
            DT_REG,
            S_IFREG | 0o644,
            InodeMeta {
                nlink: 1,
                iflags: INODE_HAS_RSRC_FORK,
                ..Default::default()
            },
        );
        b.add_xattr(oid, RESOURCE_FORK_XATTR_NAME, XATTR_DATA_STREAM, xds.as_bytes());
    }
    let xsrc = b.finish();
    let (mut out1, mut out2) = (MemTarget::default(), MemTarget::default());
    seal(&xsrc, &[], &opts, &mut out1).unwrap();
    seal(&xsrc, &[], &opts, &mut out2).unwrap();
    assert!(out1 == out2);
    let c = Container::open(&out1).unwrap();
    let fs = Fs::open(&out1, &c.volumes[0]).unwrap();
    let mut owners = Vec::new();
    fs.physical_tree(fs.sb.extentref_tree_oid.get(), &mut |k, v| {
        owners.push(PhysExtent::decode(k, v.unwrap())?.owner);
        Ok(())
    })
    .unwrap();
    assert_eq!(owners, vec![16]);

    struct FailOnDataRead {
        inner: MemTarget,
    }
    impl BlockSource for FailOnDataRead {
        fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
            if paddr >= 500 {
                return Err(io::Error::other("injected read error"));
            }
            self.inner.read_blocks(paddr, out)
        }
    }
    let dir = tempfile::tempdir().unwrap();
    let out_path = dir.path().join("partial.img");
    let failing = FailOnDataRead { inner: src };
    assert!(seal_to_file(&failing, &[], &opts, &out_path).is_err());
    assert!(!out_path.exists(), "partial output file must be removed on write error");
}
