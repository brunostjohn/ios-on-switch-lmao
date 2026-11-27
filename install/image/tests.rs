// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::io::Write;
use std::sync::Arc;

use flate2::Compression;
use flate2::write::ZlibEncoder;
use sha2::{Digest, Sha256};
use switch_apfs_alloc::Bitmap;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{
    DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME, UF_COMPRESSED, XATTR_DATA_EMBEDDED,
};
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_format::volume::{
    APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_DATA, APFS_VOL_ROLE_PREBOOT, APFS_VOL_ROLE_SYSTEM,
    ApfsSuperblock,
};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Fs, Volume};
use switch_apfs_write::place::{FreeRuns, NewContent, place};
use switch_apfs_write::records::{self, RecordInput};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::{self, VolumeIds, VolumeInput};
use switch_apfs_write::{Attrs, CheckpointAreas, Content, Tree, VolumeSpec, WriteOptions, Xattr, XattrValue};
use tempfile::TempDir;

use crate::{
    CryptexContent, CryptexImage, DATA_INCOMPAT_FEATURES, DATA_ROCOMPAT_FEATURES, DEFAULT_DATA_NAME,
    DEFAULT_PREBOOT_NAME, DEFAULT_VOLUME_FEATURES, DataVolumeParams, Error, MOBILE_GID, MOBILE_UID,
    PrebootVolumeParams, SF_IMMUTABLE, STAGING_DIR, add_file_at, build_preboot, data_volume_spec, descendant_dirs,
    ensure_dir, parse_manifest, parse_uuid, preboot_volume_spec, seed_data, split_rel, staging_name, write_file_at,
};

pub(crate) fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

fn decmpfs_header(kind: u32, size: usize) -> Vec<u8> {
    let mut out = b"fpmc".to_vec();
    out.extend_from_slice(&kind.to_le_bytes());
    out.extend_from_slice(&(size as u64).to_le_bytes());
    out
}

fn zlib(data: &[u8]) -> Vec<u8> {
    let mut enc = ZlibEncoder::new(Vec::new(), Compression::default());
    enc.write_all(data).unwrap();
    enc.finish().unwrap()
}

fn zlib_rsrc_fork(plain: &[u8]) -> Vec<u8> {
    let compressed = zlib(plain);
    let table = 4 + 8;
    let mut data = 1u32.to_le_bytes().to_vec();
    data.extend_from_slice(&(table as u32).to_le_bytes());
    data.extend_from_slice(&(compressed.len() as u32).to_le_bytes());
    data.extend_from_slice(&compressed);
    let mut fork = vec![0u8; 0x100];
    fork[..4].copy_from_slice(&0x100u32.to_be_bytes());
    fork.extend_from_slice(&(data.len() as u32).to_be_bytes());
    fork.extend_from_slice(&data);
    fork
}

pub(crate) struct SyntheticVolume {
    mem: MemTarget,
    vol: Volume,
}

impl SyntheticVolume {
    pub(crate) fn from_tree(tree: &Tree) -> Self {
        let mut used = Bitmap::new(4096);
        used.set_range(0..16);
        let mut free = FreeRuns::from_bitmap(&used);
        let placement = place(&[tree], &mut free).unwrap();

        let mut mem = MemTarget::default();
        for d in &placement.new_data {
            if let NewContent::Bytes { bytes, offset } = &d.content {
                let end = (offset + d.blocks as usize * BLOCK_SIZE).min(bytes.len());
                let mut buf = bytes[*offset..end].to_vec();
                buf.resize(d.blocks as usize * BLOCK_SIZE, 0);
                mem.write_blocks(d.dst, &buf).unwrap();
            }
        }

        let spec = VolumeSpec {
            uuid: [0x11; 16],
            name: "System".into(),
            role: APFS_VOL_ROLE_SYSTEM,
            features: DEFAULT_VOLUME_FEATURES,
            readonly_compatible_features: 0,
            incompatible_features: 0,
            unicode: UnicodeVersion::IOS_27,
        };
        let options = WriteOptions {
            target_blocks: 4096,
            areas: CheckpointAreas::Rule,
            time_ns: 1_700_000_000_000_000_000,
            formatted_by: "test".into(),
        };
        let records = records::build(&RecordInput {
            tree,
            extents: &placement.extents[0],
            incompatible_features: APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
            readonly_compatible_features: 0,
            unicode: UnicodeVersion::IOS_27,
            time_ns: options.time_ns,
        })
        .unwrap();
        let ids = VolumeIds {
            fs_index: 0,
            superblock_oid: 100,
            xid: 1,
        };
        let mut next_oid = 200;
        let mut taken = Vec::new();
        let empty_system = [0u8; BLOCK_SIZE];
        let built = volume::build(
            &VolumeInput {
                spec: &spec,
                records: &records,
                ids: &ids,
                options: &options,
                system: &empty_system,
            },
            &mut next_oid,
            &mut free,
            &mut taken,
        )
        .unwrap();
        for (paddr, block) in built.blocks {
            mem.write_blocks(paddr, &block[..]).unwrap();
        }
        let sb_paddr = built.stats.superblock;
        let sb_block = mem.block(sb_paddr).unwrap();
        let sb: ApfsSuperblock = switch_apfs_format::get(&sb_block[..], 0).unwrap();
        let vol = Volume {
            index: 0,
            oid: ids.superblock_oid,
            xid: ids.xid,
            paddr: sb_paddr,
            sb,
            block: sb_block,
        };
        Self { mem, vol }
    }

    pub(crate) fn fs(&self) -> Fs<'_> {
        Fs::open(&self.mem, &self.vol).unwrap()
    }
}

pub(crate) fn system_with_var() -> (SyntheticVolume, HashMap<&'static str, Vec<u8>>) {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let mut contents = HashMap::new();

    let plain = b"original plain bytes\n".to_vec();
    let s_plain = tree.add_bytes(plain.clone());
    add_file_at(
        &mut tree,
        "/private/var/db/plain",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o644),
        Some(s_plain),
    )
    .unwrap();
    contents.insert("/private/var/db/plain", plain);

    let inline_plain = b"inline compressed payload repeated repeated repeated\n".to_vec();
    let mut decmpfs_inline = decmpfs_header(3, inline_plain.len());
    decmpfs_inline.extend_from_slice(&zlib(&inline_plain));
    let mut comp_attrs = Attrs::new(0, 80, 0o755);
    comp_attrs.bsd_flags = UF_COMPRESSED | 0x8000;
    let inline_ino = add_file_at(
        &mut tree,
        "/private/var/db/comp_inline",
        Attrs::new(0, 0, 0o755),
        comp_attrs,
        None,
    )
    .unwrap();
    tree.put_xattr(
        inline_ino,
        Xattr {
            name: DECMPFS_XATTR_NAME.into(),
            flags: XATTR_DATA_EMBEDDED,
            value: XattrValue::Embedded(decmpfs_inline),
        },
    );
    contents.insert("/private/var/db/comp_inline", inline_plain);

    let rsrc_plain: Vec<u8> = (0..12000usize).map(|i| (i % 251) as u8).collect();
    let rsrc_hdr = decmpfs_header(4, rsrc_plain.len());
    let rsrc_fork = zlib_rsrc_fork(&rsrc_plain);
    let rsrc_fork_stream = tree.add_bytes(rsrc_fork);
    let mut rsrc_attrs = Attrs::new(0, 0, 0o755);
    rsrc_attrs.bsd_flags = UF_COMPRESSED;
    let rsrc_ino = add_file_at(
        &mut tree,
        "/private/var/db/comp_rsrc",
        Attrs::new(0, 0, 0o755),
        rsrc_attrs,
        None,
    )
    .unwrap();
    tree.put_xattr(
        rsrc_ino,
        Xattr {
            name: DECMPFS_XATTR_NAME.into(),
            flags: XATTR_DATA_EMBEDDED,
            value: XattrValue::Embedded(rsrc_hdr),
        },
    );
    tree.put_xattr(
        rsrc_ino,
        Xattr {
            name: RESOURCE_FORK_XATTR_NAME.into(),
            flags: 0x10,
            value: XattrValue::Stream(rsrc_fork_stream),
        },
    );
    contents.insert("/private/var/db/comp_rsrc", rsrc_plain);

    let hl_bytes = b"hardlinked original\n".to_vec();
    let s_hl = tree.add_bytes(hl_bytes.clone());
    let hl = add_file_at(
        &mut tree,
        "/private/var/db/hl1",
        Attrs::new(0, 0, 0o755),
        Attrs::new(64, 64, 0o640),
        Some(s_hl),
    )
    .unwrap();
    let db = tree.lookup("private/var/db").unwrap();
    tree.add_link(hl, db, "hl2").unwrap();
    contents.insert("/private/var/db/hl1", hl_bytes.clone());
    contents.insert("/private/var/db/hl2", hl_bytes);

    let app_bytes = b"staged app apple original\n".to_vec();
    let s_app = tree.add_bytes(app_bytes.clone());
    add_file_at(
        &mut tree,
        "/private/var/staged_system_apps/App.app/App",
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o755),
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o755),
        Some(s_app),
    )
    .unwrap();
    contents.insert("/private/var/staged_system_apps/App.app/App", app_bytes);

    let usr_bytes = b"system binary\n".to_vec();
    let s_usr = tree.add_bytes(usr_bytes.clone());
    add_file_at(
        &mut tree,
        "/usr/libexec/tool",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o755),
        Some(s_usr),
    )
    .unwrap();
    contents.insert("/usr/libexec/tool", usr_bytes);

    ensure_dir(
        &mut tree,
        "/private/var/mobile",
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o750),
    )
    .unwrap();

    (SyntheticVolume::from_tree(&tree), contents)
}

#[test]
fn seed_data_copies_var_and_sets_mobile_preferences() {
    let (sys, _) = system_with_var();
    let seeded = seed_data(&sys.fs()).unwrap();
    let t = &seeded.tree;

    let root = t.inode(t.root());
    assert_eq!((root.attrs.uid, root.attrs.gid, root.attrs.mode), (0, 0, 0o755));
    assert!(t.lookup("db/plain").is_some());
    assert!(t.lookup("staged_system_apps/App.app/App").is_some());
    assert!(t.lookup("usr/libexec/tool").is_none());

    let mobile = t.inode(t.lookup("mobile").unwrap());
    assert_eq!(
        (mobile.attrs.uid, mobile.attrs.gid, mobile.attrs.mode),
        (501, 501, 0o750)
    );
    for p in ["mobile/Library", "mobile/Library/Preferences"] {
        let node = t.inode(t.lookup(p).unwrap());
        assert_eq!((node.attrs.uid, node.attrs.gid, node.attrs.mode), (501, 501, 0o755));
    }
}

#[test]
fn seed_data_reowns_existing_mobile_library() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    ensure_dir(&mut tree, "/private/var/mobile/Library", Attrs::new(0, 0, 0o700)).unwrap();
    let sys = SyntheticVolume::from_tree(&tree);
    let seeded = seed_data(&sys.fs()).unwrap();
    let lib = seeded.tree.inode(seeded.tree.lookup("mobile/Library").unwrap());
    assert_eq!((lib.attrs.uid, lib.attrs.gid, lib.attrs.mode), (501, 501, 0o700));
    let prefs = seeded
        .tree
        .inode(seeded.tree.lookup("mobile/Library/Preferences").unwrap());
    assert_eq!((prefs.attrs.uid, prefs.attrs.gid, prefs.attrs.mode), (501, 501, 0o755));
}

#[test]
fn preboot_stages_single_extent_fills_and_verifies_sha256() {
    let dir = TempDir::new().unwrap();
    let payload: Vec<u8> = (0..10_000usize).map(|i| (i * 13 % 251) as u8).collect();
    let digest = sha256(&payload);
    let file_path = dir.path().join("sys.dmg");
    std::fs::write(&file_path, &payload).unwrap();

    let tree = build_preboot(&[
        CryptexImage {
            name: "sys.dmg".into(),
            content: CryptexContent::File {
                path: file_path.clone(),
                expected_sha256: Some(digest),
            },
        },
        CryptexImage {
            name: "app.dmg".into(),
            content: CryptexContent::Bytes {
                bytes: Arc::from(payload.clone()),
                expected_sha256: Some(digest),
            },
        },
    ])
    .unwrap();

    let root = tree.inode(tree.root());
    assert_eq!((root.attrs.uid, root.attrs.gid, root.attrs.mode), (0, 0, 0o755));
    let staging = tree.inode(tree.lookup(STAGING_DIR).unwrap());
    assert_eq!(
        (staging.attrs.uid, staging.attrs.gid, staging.attrs.mode),
        (0, 0, 0o755)
    );

    let mut used = Bitmap::new(1024);
    used.set_range(0..10);
    // Fragment free space so a 2-block hole sits before a larger contiguous run.
    used.set_range(12..13);
    let mut free = FreeRuns::from_bitmap(&used);
    let placed = place(&[&tree], &mut free).unwrap();

    for (idx, name) in ["Switch-Staging/sys.dmg", "Switch-Staging/app.dmg"].iter().enumerate() {
        let ino = tree.inode(tree.lookup(name).unwrap());
        assert_eq!((ino.attrs.uid, ino.attrs.gid, ino.attrs.mode), (0, 0, 0o644));
        assert_eq!(placed.extents[0][idx].len(), 1, "{name} must be one contiguous extent");
        assert!(placed.extents[0][idx][0].paddr >= 13);
        let Content::Fill(fill) = &tree.stream(ino.data.unwrap()).content else {
            panic!("{name} must use Content::Fill");
        };
        let mut target = MemTarget::default();
        fill.fill(&mut target).unwrap();
        let mut read_back = Vec::new();
        for b in 0..3u64 {
            read_back.extend_from_slice(&target.blocks[&b][..]);
        }
        assert_eq!(&read_back[..payload.len()], &payload[..]);
        assert!(read_back[payload.len()..].iter().all(|&b| b == 0));
    }

    let bad_bytes = build_preboot(&[CryptexImage {
        name: "app.dmg".into(),
        content: CryptexContent::Bytes {
            bytes: Arc::from(payload),
            expected_sha256: Some([0xFF; 32]),
        },
    }]);
    assert!(matches!(bad_bytes, Err(Error::DigestMismatch { .. })));

    let bad_file = build_preboot(&[CryptexImage {
        name: "sys.dmg".into(),
        content: CryptexContent::File {
            path: file_path,
            expected_sha256: Some([0xFF; 32]),
        },
    }])
    .unwrap();
    let ino = bad_file.inode(bad_file.lookup("Switch-Staging/sys.dmg").unwrap());
    let Content::Fill(fill) = &bad_file.stream(ino.data.unwrap()).content else {
        panic!("expected Fill");
    };
    let mut target = MemTarget::default();
    assert!(fill.fill(&mut target).is_err());

    assert_eq!(
        staging_name("/private/preboot/Switch-Staging/044-76034-038.dmg").unwrap(),
        "044-76034-038.dmg"
    );
    assert_eq!(staging_name("044-76034-038.dmg").unwrap(), "044-76034-038.dmg");
    assert!(staging_name("").is_err());
    assert!(staging_name(".").is_err());
    assert!(staging_name("..").is_err());
    assert!(staging_name("sub/a.dmg").is_err());
    assert!(staging_name("/private/preboot/other/a.dmg").is_err());
    assert!(staging_name("/private/preboot/Switch-Staging/sub/a.dmg").is_err());
}

#[test]
fn volume_specs_and_uuid_parsing() {
    let data_uuid = parse_uuid("D1B1D053-D0A6-496C-8625-A947A3DA863E").unwrap();
    let preboot_uuid = parse_uuid("4aa4a761-5138-439d-9a30-d4138f6887fa").unwrap();
    assert!(parse_uuid("D1B1D053D0A6496C8625A947A3DA863E").is_err());
    assert!(parse_uuid("not-a-uuid").is_err());

    let data_spec = data_volume_spec(&DataVolumeParams::new(
        data_uuid,
        DEFAULT_DATA_NAME,
        UnicodeVersion { major: 15, minor: 1 },
    ));
    assert_eq!(data_spec.role, APFS_VOL_ROLE_DATA);
    assert_eq!(data_spec.features, DEFAULT_VOLUME_FEATURES);
    assert_eq!(data_spec.readonly_compatible_features, DATA_ROCOMPAT_FEATURES);
    assert_eq!(data_spec.incompatible_features, DATA_INCOMPAT_FEATURES);
    assert_eq!(data_spec.unicode, UnicodeVersion { major: 15, minor: 1 });

    let preboot_spec = preboot_volume_spec(&PrebootVolumeParams::new(
        preboot_uuid,
        DEFAULT_PREBOOT_NAME,
        UnicodeVersion { major: 17, minor: 0 },
    ));
    assert_eq!(preboot_spec.role, APFS_VOL_ROLE_PREBOOT);
    assert_eq!(preboot_spec.features, DEFAULT_VOLUME_FEATURES);
    assert_eq!(preboot_spec.readonly_compatible_features, 0);
    assert_eq!(preboot_spec.incompatible_features, 0);
    assert_eq!(preboot_spec.unicode, UnicodeVersion { major: 17, minor: 0 });
}

#[test]
fn path_helpers_and_manifest_refusals() {
    assert_eq!(split_rel("").unwrap(), Vec::<&str>::new());
    assert_eq!(split_rel("/").unwrap(), Vec::<&str>::new());
    assert_eq!(split_rel("/a/b/c").unwrap(), vec!["a", "b", "c"]);
    for bad in ["/a//b", "/a/../b", "/a/./b", "a/", "a\0b"] {
        assert!(split_rel(bad).is_err(), "{bad} should fail");
    }

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let sub = ensure_dir(&mut tree, "Switch/pretranslated", Attrs::new(0, 0, 0o755)).unwrap();
    let deep = ensure_dir(&mut tree, "Switch/pretranslated/usr/libexec", Attrs::new(0, 0, 0o755)).unwrap();
    let s1 = tree.add_bytes(&b"v1"[..]);
    let f = write_file_at(
        &mut tree,
        "Switch/pretranslated/usr/libexec/x",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o644),
        Some(s1),
    )
    .unwrap();
    let s2 = tree.add_bytes(&b"v22"[..]);
    let mut updated = Attrs::new(0, 0, 0o755);
    updated.bsd_flags = SF_IMMUTABLE;
    let f2 = write_file_at(
        &mut tree,
        "Switch/pretranslated/usr/libexec/x",
        Attrs::new(0, 0, 0o755),
        updated,
        Some(s2),
    )
    .unwrap();
    assert_eq!(f, f2);
    assert_eq!(tree.inode(f).attrs.bsd_flags, SF_IMMUTABLE);
    assert_eq!(tree.stream(tree.inode(f).data.unwrap()).size, 3);

    let dirs = descendant_dirs(&tree, sub).unwrap();
    assert_eq!(dirs.len(), 3);
    assert!(dirs.contains(&sub));
    assert!(dirs.contains(&deep));
    assert!(!dirs.contains(&tree.lookup("Switch").unwrap()));

    assert!(parse_manifest(br#"{"files":[{"path":"rel","file":"f","sha256_in":"00","sha256_out":"00"}]}"#).is_err());
    assert!(
        parse_manifest(
            br#"{"files":[{"path":"/a/../b","file":"files/a","sha256_in":"0000000000000000000000000000000000000000000000000000000000000000","sha256_out":"0000000000000000000000000000000000000000000000000000000000000000"}]}"#
        )
        .is_err()
    );
}
