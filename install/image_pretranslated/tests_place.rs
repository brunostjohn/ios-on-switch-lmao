// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_alloc::Bitmap;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::UF_COMPRESSED;
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_format::volume::{APFS_INCOMPAT_NORMALIZATION_INSENSITIVE, APFS_VOL_ROLE_SYSTEM, ApfsSuperblock};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Fs, Volume};
use switch_apfs_write::place::{FreeRuns, NewContent, place};
use switch_apfs_write::records::{self, RecordInput};
use switch_apfs_write::target::MemTarget;
use switch_apfs_write::volume::{self, VolumeIds, VolumeInput};
use switch_apfs_write::{Attrs, CheckpointAreas, Tree, VolumeSpec, WriteOptions};
use switch_image::{
    DEFAULT_VOLUME_FEATURES, DeltaEntry, MOBILE_GID, MOBILE_UID, Replacement, SF_IMMUTABLE, add_file_at, ensure_dir,
    seed_data, tree_entries,
};

use crate::overlay_index::{self, IndexEntry, OriginalsPair};
use crate::{
    Error, OVERLAY_DIR, OVERLAY_INDEX, OVERLAY_MANIFEST, OVERLAY_ORIGINALS, parse_cdhash_pairs, place_pretranslated,
};

fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

struct SyntheticVolume {
    mem: MemTarget,
    vol: Volume,
}

impl SyntheticVolume {
    fn from_tree(tree: &Tree) -> Self {
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
            uuid: [0x22; 16],
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

    fn fs(&self) -> Fs<'_> {
        Fs::open(&self.mem, &self.vol).unwrap()
    }
}

#[test]
fn place_pretranslated_hardlinks_staged_apps_flags_and_inodes() {
    let mut sys_tree = Tree::new(Attrs::new(0, 0, 0o755));
    ensure_dir(
        &mut sys_tree,
        "/private/var/mobile",
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o755),
    )
    .unwrap();

    let tool_orig = b"orig tool".to_vec();
    let s_tool = sys_tree.add_bytes(tool_orig.clone());
    let mut tool_attrs = Attrs::new(0, 0, 0o4755);
    tool_attrs.bsd_flags = UF_COMPRESSED | 0x8000;
    add_file_at(
        &mut sys_tree,
        "/usr/libexec/tool",
        Attrs::new(0, 0, 0o755),
        tool_attrs,
        Some(s_tool),
    )
    .unwrap();

    let hl_orig = b"orig hardlink".to_vec();
    let s_hl = sys_tree.add_bytes(hl_orig.clone());
    let hl_ino = add_file_at(
        &mut sys_tree,
        "/usr/libexec/hl_a",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 80, 0o775),
        Some(s_hl),
    )
    .unwrap();
    let sbin = ensure_dir(&mut sys_tree, "/usr/sbin", Attrs::new(0, 0, 0o755)).unwrap();
    sys_tree.add_link(hl_ino, sbin, "hl_b").unwrap();

    let app_orig = b"orig staged app".to_vec();
    let s_app = sys_tree.add_bytes(app_orig.clone());
    add_file_at(
        &mut sys_tree,
        "/private/var/staged_system_apps/App.app/App",
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o755),
        Attrs::new(MOBILE_UID, MOBILE_GID, 0o755),
        Some(s_app),
    )
    .unwrap();

    let sys = SyntheticVolume::from_tree(&sys_tree);
    let fs = sys.fs();
    let mut data = seed_data(&fs).unwrap().tree;

    let tool_new = b"translated tool".to_vec();
    let hl_new = b"translated hl".to_vec();
    let app_new = b"translated app".to_vec();
    let cd_tool = [0x11; 20];
    let cd_hl = [0x22; 20];
    let cd_app_orig = [0x33; 20];
    let cd_app_new = [0x44; 20];

    let repl: HashMap<String, Vec<u8>> = HashMap::from([
        ("/usr/libexec/tool".into(), tool_new.clone()),
        ("/usr/libexec/hl_a".into(), hl_new.clone()),
        ("/usr/sbin/hl_b".into(), hl_new.clone()),
        ("/private/var/staged_system_apps/App.app/App".into(), app_new.clone()),
    ]);
    let all_entries = vec![
        DeltaEntry {
            path: "/usr/libexec/tool".into(),
            file: "files/usr/libexec/tool".into(),
            sha256_in: sha256(&tool_orig),
            sha256_out: sha256(&tool_new),
            cdhash: vec![hex::encode(cd_tool)],
        },
        DeltaEntry {
            path: "/usr/libexec/hl_a".into(),
            file: "files/usr/libexec/hl_a".into(),
            sha256_in: sha256(&hl_orig),
            sha256_out: sha256(&hl_new),
            cdhash: vec![hex::encode(cd_hl)],
        },
        DeltaEntry {
            path: "/usr/sbin/hl_b".into(),
            file: "files/usr/sbin/hl_b".into(),
            sha256_in: sha256(&hl_orig),
            sha256_out: sha256(&hl_new),
            cdhash: vec![hex::encode(cd_hl)],
        },
        DeltaEntry {
            path: "/private/var/staged_system_apps/App.app/App".into(),
            file: "files/private/var/staged_system_apps/App.app/App".into(),
            sha256_in: sha256(&app_orig),
            sha256_out: sha256(&app_new),
            cdhash: vec![hex::encode(cd_app_new)],
        },
    ];
    let pairs_text = format!(
        "{} {}\n{} {}\n",
        hex::encode(cd_app_orig),
        hex::encode(cd_app_new),
        hex::encode(cd_app_orig),
        hex::encode(cd_app_new),
    );
    let pairs = parse_cdhash_pairs(&pairs_text).unwrap();
    let loader = |e: &DeltaEntry| Ok(Replacement::Bytes(Arc::from(repl[&e.path].clone())));

    let placed = place_pretranslated(&mut data, &fs, &tree_entries(&all_entries), &loader, &pairs).unwrap();
    assert_eq!(placed.placed_files, 3);
    assert_eq!(placed.hard_links, 1);

    let switch_dir = data.inode(data.lookup("Switch").unwrap());
    assert_eq!(
        (
            switch_dir.attrs.uid,
            switch_dir.attrs.gid,
            switch_dir.attrs.mode,
            switch_dir.attrs.bsd_flags,
        ),
        (0, 0, 0o755, 0)
    );

    let tool_ref = data.lookup("Switch/pretranslated/usr/libexec/tool").unwrap();
    let tool = data.inode(tool_ref);
    assert_eq!((tool.attrs.uid, tool.attrs.gid, tool.attrs.mode), (0, 0, 0o4755));
    assert_eq!(tool.attrs.bsd_flags, 0x8000 | SF_IMMUTABLE);

    let hl_ref = data.lookup("Switch/pretranslated/usr/libexec/hl_a").unwrap();
    let hl = data.inode(hl_ref);
    assert_eq!(
        (hl.attrs.uid, hl.attrs.gid, hl.attrs.mode, hl.attrs.bsd_flags),
        (0, 80, 0o775, SF_IMMUTABLE)
    );
    assert!(data.lookup("Switch/pretranslated/usr/sbin/hl_b").is_none());

    let app_ref = data
        .lookup("Switch/pretranslated/private/var/staged_system_apps/App.app/App")
        .unwrap();
    let app = data.inode(app_ref);
    assert_eq!(
        (app.attrs.uid, app.attrs.gid, app.attrs.mode, app.attrs.bsd_flags),
        (0, 0, 0o755, SF_IMMUTABLE)
    );

    for meta in [OVERLAY_MANIFEST, OVERLAY_INDEX, OVERLAY_ORIGINALS] {
        let node = data.inode(data.lookup(meta).unwrap());
        assert_eq!(
            (node.attrs.uid, node.attrs.gid, node.attrs.mode, node.attrs.bsd_flags),
            (0, 0, 0o644, SF_IMMUTABLE)
        );
    }

    let (idx_files, idx_dirs) = overlay_index::decode(&placed.index_bytes).unwrap();
    assert_eq!(idx_files.len(), 4);
    let by_path: HashMap<&str, &IndexEntry> = idx_files.iter().map(|e| (e.path.as_str(), e)).collect();
    assert_eq!(
        by_path["/usr/libexec/tool"].system_id,
        fs.lookup("/usr/libexec/tool").unwrap().unwrap()
    );
    assert_eq!(by_path["/usr/libexec/tool"].data_id, data.number(tool_ref));
    assert_eq!(by_path["/usr/libexec/hl_a"].data_id, data.number(hl_ref));
    assert_eq!(by_path["/usr/sbin/hl_b"].data_id, data.number(hl_ref));
    assert_eq!(
        by_path["/usr/sbin/hl_b"].system_id,
        by_path["/usr/libexec/hl_a"].system_id
    );
    assert_eq!(
        by_path["/private/var/staged_system_apps/App.app/App"].data_id,
        data.number(app_ref)
    );

    let pt_root = data.lookup(OVERLAY_DIR).unwrap();
    assert!(idx_dirs.contains(&data.number(pt_root)));
    assert_eq!(idx_dirs.len(), placed.directories);
    for dir_path in [
        "Switch/pretranslated",
        "Switch/pretranslated/usr",
        "Switch/pretranslated/usr/libexec",
        "Switch/pretranslated/private",
        "Switch/pretranslated/private/var",
        "Switch/pretranslated/private/var/staged_system_apps",
        "Switch/pretranslated/private/var/staged_system_apps/App.app",
    ] {
        let r = data.lookup(dir_path).unwrap();
        let node = data.inode(r);
        assert_eq!(
            (node.attrs.uid, node.attrs.gid, node.attrs.mode, node.attrs.bsd_flags),
            (0, 0, 0o755, SF_IMMUTABLE)
        );
        assert!(idx_dirs.contains(&data.number(r)), "missing dir id for {dir_path}");
    }

    let originals = overlay_index::decode_originals(placed.originals_bytes.as_ref().unwrap()).unwrap();
    assert_eq!(
        originals,
        vec![OriginalsPair {
            original: cd_app_orig,
            translated: cd_app_new,
        }]
    );
    assert_eq!(placed.manifest_json, placed.manifest.to_json().as_bytes());
}

#[test]
fn place_pretranslated_refusals() {
    let mut sys_tree = Tree::new(Attrs::new(0, 0, 0o755));
    ensure_dir(&mut sys_tree, "/private/var/mobile", Attrs::new(501, 501, 0o755)).unwrap();
    let s = sys_tree.add_bytes(&b"bin"[..]);
    let hl = add_file_at(
        &mut sys_tree,
        "/usr/bin/a",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o755),
        Some(s),
    )
    .unwrap();
    let dir = sys_tree.lookup("usr/bin").unwrap();
    sys_tree.add_link(hl, dir, "b").unwrap();
    sys_tree.add_symlink(dir, "sym", Attrs::new(0, 0, 0o755), "a").unwrap();
    let s_app = sys_tree.add_bytes(&b"app"[..]);
    add_file_at(
        &mut sys_tree,
        "/private/var/staged_system_apps/X.app/X",
        Attrs::new(501, 501, 0o755),
        Attrs::new(501, 501, 0o755),
        Some(s_app),
    )
    .unwrap();

    let sys = SyntheticVolume::from_tree(&sys_tree);
    let fs = sys.fs();

    let loader_ok = |_: &DeltaEntry| Ok(Replacement::Bytes(Arc::from(b"out".to_vec())));
    let sym_entry = vec![DeltaEntry {
        path: "/usr/bin/sym".into(),
        file: "files/usr/bin/sym".into(),
        sha256_in: sha256(b"bin"),
        sha256_out: sha256(b"out"),
        cdhash: vec![hex::encode([1; 20])],
    }];
    let mut data = seed_data(&fs).unwrap().tree;
    assert!(matches!(
        place_pretranslated(&mut data, &fs, &sym_entry, &loader_ok, &[]),
        Err(Error::NotRegularFile(_))
    ));

    let conflict_hl = vec![
        DeltaEntry {
            path: "/usr/bin/a".into(),
            file: "files/usr/bin/a".into(),
            sha256_in: sha256(b"bin"),
            sha256_out: sha256(b"out"),
            cdhash: vec![hex::encode([1; 20])],
        },
        DeltaEntry {
            path: "/usr/bin/b".into(),
            file: "files/usr/bin/b".into(),
            sha256_in: sha256(b"bin"),
            sha256_out: sha256(b"out"),
            cdhash: vec![hex::encode([2; 20])],
        },
    ];
    let mut data = seed_data(&fs).unwrap().tree;
    assert!(matches!(
        place_pretranslated(&mut data, &fs, &conflict_hl, &loader_ok, &[]),
        Err(Error::ConflictingTranslation { .. })
    ));

    let staged_missing_pair = vec![DeltaEntry {
        path: "/private/var/staged_system_apps/X.app/X".into(),
        file: "files/private/var/staged_system_apps/X.app/X".into(),
        sha256_in: sha256(b"app"),
        sha256_out: sha256(b"out"),
        cdhash: vec![hex::encode([9; 20])],
    }];
    let mut data = seed_data(&fs).unwrap().tree;
    assert!(matches!(
        place_pretranslated(&mut data, &fs, &staged_missing_pair, &loader_ok, &[]),
        Err(Error::MissingOriginalCdhash(_))
    ));

    let conflicting_pairs = [
        OriginalsPair {
            original: [1; 20],
            translated: [9; 20],
        },
        OriginalsPair {
            original: [2; 20],
            translated: [9; 20],
        },
    ];
    let mut data = seed_data(&fs).unwrap().tree;
    assert!(matches!(
        place_pretranslated(&mut data, &fs, &staged_missing_pair, &loader_ok, &conflicting_pairs),
        Err(Error::ConflictingCdhashPair { .. })
    ));

    assert!(matches!(
        parse_cdhash_pairs("only_one_token\n"),
        Err(Error::InvalidCdhashPairLine(_))
    ));
    assert!(matches!(
        parse_cdhash_pairs("abcd ef01\n"),
        Err(Error::InvalidCdhashPairLine(_))
    ));
}

#[test]
fn place_pretranslated_without_staged_apps_omits_originals() {
    let mut sys_tree = Tree::new(Attrs::new(0, 0, 0o755));
    ensure_dir(&mut sys_tree, "/private/var/mobile", Attrs::new(501, 501, 0o755)).unwrap();
    let s = sys_tree.add_bytes(&b"orig"[..]);
    add_file_at(
        &mut sys_tree,
        "/usr/bin/plain",
        Attrs::new(0, 0, 0o755),
        Attrs::new(0, 0, 0o755),
        Some(s),
    )
    .unwrap();
    let sys = SyntheticVolume::from_tree(&sys_tree);
    let fs = sys.fs();

    let mut data = seed_data(&fs).unwrap().tree;
    let out_bytes = b"translated".to_vec();
    let entries = vec![DeltaEntry {
        path: "/usr/bin/plain".into(),
        file: "files/usr/bin/plain".into(),
        sha256_in: sha256(b"orig"),
        sha256_out: sha256(&out_bytes),
        cdhash: vec![hex::encode([7; 20])],
    }];
    let loader = |_: &DeltaEntry| Ok(Replacement::Bytes(Arc::from(out_bytes.clone())));
    let placed = place_pretranslated(&mut data, &fs, &entries, &loader, &[]).unwrap();
    assert!(placed.originals_bytes.is_none());
    assert!(data.lookup(OVERLAY_ORIGINALS).is_none());
    assert!(data.lookup(OVERLAY_INDEX).is_some());

    let mut empty_data = seed_data(&fs).unwrap().tree;
    let empty = place_pretranslated(&mut empty_data, &fs, &[], &loader, &[]).unwrap();
    assert_eq!(empty.manifest_json, b"{\n \"files\": []\n}\n");
    assert!(empty.originals_bytes.is_none());
    let (idx_files, idx_dirs) = overlay_index::decode(&empty.index_bytes).unwrap();
    assert!(idx_files.is_empty());
    assert_eq!(
        idx_dirs,
        vec![empty_data.number(empty_data.lookup(OVERLAY_DIR).unwrap())]
    );
}
