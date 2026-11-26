// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use switch_apfs_format::fs_records::SYMLINK_XATTR_NAME;
use switch_apfs_write::{Attrs, Content, Kind, Tree, XattrValue};

use crate::{
    ARCHIVE, BootstrapInput, BootstrapPins, DIRECTORY, HELD, HELPER, MANIFEST, METADATA, MIGRATION_PENDING,
    parse_control, stage_bootstrap, stage_bootstrap_from_dir,
};

pub(crate) const SAMPLE_STATUS: &str = concat!(
    "Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n\n",
    "Package: ellekit\nStatus: install ok installed\nVersion: 1.1\n\n",
    "Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n",
    "Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n\n",
    "Package: firmware\nStatus: install ok installed\nVersion: 15.0\n",
);

pub(crate) struct TarSpec<'a> {
    pub name: &'a str,
    pub kind: tar::EntryType,
    pub mode: u32,
    pub uid: u64,
    pub gid: u64,
    pub data: &'a [u8],
    pub link: Option<&'a str>,
}

pub(crate) fn reg<'a>(name: &'a str, data: &'a [u8], mode: u32, uid: u64, gid: u64) -> TarSpec<'a> {
    TarSpec {
        name,
        kind: tar::EntryType::Regular,
        mode,
        uid,
        gid,
        data,
        link: None,
    }
}

pub(crate) fn sym<'a>(name: &'a str, target: &'a str) -> TarSpec<'a> {
    TarSpec {
        name,
        kind: tar::EntryType::Symlink,
        mode: 0o777,
        uid: 0,
        gid: 0,
        data: b"",
        link: Some(target),
    }
}

pub(crate) fn hard<'a>(name: &'a str, target: &'a str, mode: u32, uid: u64, gid: u64) -> TarSpec<'a> {
    TarSpec {
        name,
        kind: tar::EntryType::Link,
        mode,
        uid,
        gid,
        data: b"",
        link: Some(target),
    }
}

pub(crate) fn make_tar(specs: &[TarSpec<'_>]) -> Vec<u8> {
    let mut builder = tar::Builder::new(Vec::new());
    for spec in specs {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(spec.kind);
        header.set_mode(spec.mode);
        header.set_uid(spec.uid);
        header.set_gid(spec.gid);
        header.set_size(if spec.kind.is_file() { spec.data.len() as u64 } else { 0 });
        header.set_mtime(0);
        if let Some(target) = spec.link {
            builder.append_link(&mut header, spec.name, target).unwrap();
        } else {
            builder.append_data(&mut header, spec.name, spec.data).unwrap();
        }
    }
    builder.into_inner().unwrap()
}

pub(crate) fn make_package_bytes(
    extra: &[TarSpec<'_>],
    helper: &[u8],
    extra_manifest: Option<Value>,
) -> (Vec<u8>, Vec<u8>) {
    let mut specs = vec![
        reg(".procursus_strapped", b"", 0o644, 0, 0),
        reg(".switch-bootstrap-ready", b"", 0o644, 0, 0),
        reg("Library/dpkg/status", SAMPLE_STATUS.as_bytes(), 0o644, 0, 0),
        reg("usr/bin/tool", b"tool", 0o755, 0, 0),
    ];
    for s in extra {
        specs.push(TarSpec {
            name: s.name,
            kind: s.kind,
            mode: s.mode,
            uid: s.uid,
            gid: s.gid,
            data: s.data,
            link: s.link,
        });
    }
    let archive = make_tar(&specs);
    let mut manifest = json!({
        "schema_version": 1,
        "archive_sha256": hex::encode(Sha256::digest(&archive)),
        "helper_sha256": hex::encode(Sha256::digest(helper)),
        "holds": HELD,
    });
    if let Some(Value::Object(extra_map)) = extra_manifest {
        let map = manifest.as_object_mut().unwrap();
        for (k, v) in extra_map {
            map.insert(k, v);
        }
    }
    (serde_json::to_vec(&manifest).unwrap(), archive)
}

fn read_bytes(tree: &Tree, path: &str) -> Vec<u8> {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    let node = tree.inode(inode);
    assert_eq!(node.kind, Kind::File);
    match node.data {
        None => Vec::new(),
        Some(s) => match &tree.stream(s).content {
            Content::Bytes(b) => b.to_vec(),
            other => panic!("unexpected {other:?}"),
        },
    }
}

fn read_symlink(tree: &Tree, path: &str) -> String {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    let node = tree.inode(inode);
    assert_eq!(node.kind, Kind::Symlink);
    let x = node.xattrs.iter().find(|x| x.name == SYMLINK_XATTR_NAME).unwrap();
    let XattrValue::Embedded(bytes) = &x.value else {
        panic!("symlink target not embedded");
    };
    String::from_utf8(bytes.strip_suffix(&[0]).unwrap().to_vec()).unwrap()
}

#[test]
fn both_builds_use_pinned_firmware_and_distinct_package_state() {
    let helper = b"helper";
    let (manifest, archive) = make_package_bytes(&[], helper, None);
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));

    for (build, version) in [("21H461", "17.7.11"), ("24B119", "27.2")] {
        let pins = BootstrapPins { build, version };
        let input = BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: Some(helper),
        };
        let report = stage_bootstrap(&mut tree, &input, &pins).unwrap();
        assert_eq!(report.build, build);
        assert_eq!(report.firmware, version);
        assert!(report.migration_pending.is_none());

        let base = format!("{DIRECTORY}/{build}");
        let state: Value = serde_json::from_slice(&read_bytes(&tree, &format!("{base}/{MANIFEST}"))).unwrap();
        assert_eq!(state["build"], build);
        assert_eq!(state["firmware"], version);

        let status_text = String::from_utf8(read_bytes(&tree, &format!("{base}/root/Library/dpkg/status"))).unwrap();
        let records = parse_control(&status_text).unwrap();
        let by_pkg: BTreeMap<&str, _> = records.iter().map(|r| (r.get("Package").unwrap(), r)).collect();
        assert_eq!(by_pkg["firmware"].get("Version"), Some(version));
        for &held in &HELD {
            assert_eq!(by_pkg[held].get("Status"), Some("hold ok installed"));
        }
        assert_eq!(
            read_symlink(&tree, "jb"),
            format!("/private/var/{DIRECTORY}/{build}/root")
        );
    }
    assert!(tree.lookup("Switch/bootstrap/21H461/root/usr/bin/tool").is_some());
    assert!(tree.lookup("Switch/bootstrap/24B119/root/usr/bin/tool").is_some());
}

#[test]
fn managed_generation_writes_migration_pending_and_not_native_guards() {
    let helper = b"helper";
    let gen_digest = "b".repeat(64);
    let (manifest, archive) = make_package_bytes(&[], helper, Some(json!({ "managed_generation": gen_digest })));

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };
    let report = stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap();
    assert_eq!(report.migration_pending.as_deref(), Some(gen_digest.as_str()));

    let pending_path = format!("{DIRECTORY}/21H461/{MIGRATION_PENDING}");
    let pending_inode = tree.lookup(&pending_path).unwrap();
    let attrs = &tree.inode(pending_inode).attrs;
    assert_eq!((attrs.uid, attrs.gid, attrs.mode), (0, 0, 0o644));
    assert_eq!(read_bytes(&tree, &pending_path), format!("{gen_digest}\n").into_bytes());

    assert!(tree.lookup("Switch/system-app-migration").is_none());
    assert!(tree.lookup("datamigrator/migrate").is_none());
}

#[test]
fn symlinks_hard_links_modes_and_numeric_ownership_survive() {
    let helper = b"helper";
    let extra = [
        reg("var/cache/apt/.keep", b"", 0o644, 501, 501),
        reg("usr/bin/suid", b"suid", 0o4755, 0, 0),
        sym("usr/bin/tool-alias", "tool"),
        hard("usr/bin/hard", "usr/bin/tool", 0o755, 0, 0),
    ];
    let (manifest, archive) = make_package_bytes(&extra, helper, None);
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };
    stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap();

    let root = "Switch/bootstrap/21H461/root";
    assert_eq!(read_symlink(&tree, &format!("{root}/usr/bin/tool-alias")), "tool");

    let tool_ref = tree.lookup(&format!("{root}/usr/bin/tool")).unwrap();
    let hard_ref = tree.lookup(&format!("{root}/usr/bin/hard")).unwrap();
    assert_eq!(tool_ref, hard_ref);
    assert_eq!(tree.inode(tool_ref).links.len(), 2);
    assert_eq!(tree.inode(tool_ref).attrs.mode, 0o755);

    let suid = tree.inode(tree.lookup(&format!("{root}/usr/bin/suid")).unwrap());
    assert_eq!((suid.attrs.uid, suid.attrs.gid, suid.attrs.mode), (0, 0, 0o4755));

    let keep = tree.inode(tree.lookup(&format!("{root}/var/cache/apt/.keep")).unwrap());
    assert_eq!((keep.attrs.uid, keep.attrs.gid, keep.attrs.mode), (501, 501, 0o644));
    assert!(keep.data.is_none());

    let meta_inode = tree.lookup(&format!("{DIRECTORY}/21H461/{METADATA}")).unwrap();
    assert_eq!(
        (
            tree.inode(meta_inode).attrs.uid,
            tree.inode(meta_inode).attrs.gid,
            tree.inode(meta_inode).attrs.mode
        ),
        (0, 0, 0o644)
    );
}

#[test]
fn helper_lookup_from_overlay_in_tree_and_tampering_refusal() {
    let helper = b"real-helper";
    let (manifest, archive) = make_package_bytes(&[], helper, None);
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let sw = tree.add_dir(tree.root(), "Switch", Attrs::new(0, 0, 0o755)).unwrap();
    let ov = tree.add_dir(sw, "overlay", Attrs::new(0, 0, 0o755)).unwrap();
    let usr = tree.add_dir(ov, "usr", Attrs::new(0, 0, 0o755)).unwrap();
    let libexec = tree.add_dir(usr, "libexec", Attrs::new(0, 0, 0o755)).unwrap();
    let s = tree.add_bytes(&helper[..]);
    let helper_inode = tree
        .add_file(libexec, HELPER, Attrs::new(0, 0, 0o755), Some(s))
        .unwrap();

    stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: None,
        },
        &pins,
    )
    .unwrap();

    let mut tree2 = Tree::new(Attrs::new(0, 0, 0o755));
    let sw = tree2.add_dir(tree2.root(), "Switch", Attrs::new(0, 0, 0o755)).unwrap();
    let ov = tree2.add_dir(sw, "overlay", Attrs::new(0, 0, 0o755)).unwrap();
    let usr = tree2.add_dir(ov, "usr", Attrs::new(0, 0, 0o755)).unwrap();
    let libexec = tree2.add_dir(usr, "libexec", Attrs::new(0, 0, 0o755)).unwrap();
    let s2 = tree2.add_bytes(&b"tampered"[..]);
    tree2
        .add_file(libexec, HELPER, Attrs::new(0, 0, 0o755), Some(s2))
        .unwrap();
    let err = stage_bootstrap(
        &mut tree2,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: None,
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("SHA-256"));
    assert!(tree2.lookup("Switch/bootstrap").is_none());
    assert!(tree2.lookup("jb").is_none());
    let _ = helper_inode;
}

#[test]
fn hash_build_firmware_and_tree_precondition_mismatches_are_refused() {
    let helper = b"helper";
    let (manifest, mut archive) = make_package_bytes(&[], helper, None);
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };

    archive.extend_from_slice(b"tampered");
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let err = stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("SHA-256"));
    assert!(tree.lookup("Switch").is_none());

    let (bad_build_manifest, good_archive) = make_package_bytes(&[], helper, Some(json!({ "build": "24B119" })));
    let err = stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &bad_build_manifest,
            archive: &good_archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("differs from selected IPSW"));

    let (bad_fw_manifest, good_archive) = make_package_bytes(&[], helper, Some(json!({ "firmware": "18.0" })));
    let err = stage_bootstrap(
        &mut tree,
        &BootstrapInput {
            manifest_json: &bad_fw_manifest,
            archive: &good_archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("differs from selected IPSW"));

    let mut tree_jb_dir = Tree::new(Attrs::new(0, 0, 0o755));
    tree_jb_dir
        .add_dir(tree_jb_dir.root(), "jb", Attrs::new(0, 0, 0o755))
        .unwrap();
    let err = stage_bootstrap(
        &mut tree_jb_dir,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &good_archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("/var/jb already exists"));
    assert!(tree_jb_dir.lookup("Switch").is_none());

    let mut tree_jb_sym = Tree::new(Attrs::new(0, 0, 0o755));
    tree_jb_sym
        .add_symlink(tree_jb_sym.root(), "jb", Attrs::new(0, 0, 0o755), "/private/var/other")
        .unwrap();
    let err = stage_bootstrap(
        &mut tree_jb_sym,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &good_archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("outside the managed bootstrap"));

    let mut tree_sw_sym = Tree::new(Attrs::new(0, 0, 0o755));
    tree_sw_sym
        .add_symlink(tree_sw_sym.root(), "Switch", Attrs::new(0, 0, 0o755), "/outside")
        .unwrap();
    let err = stage_bootstrap(
        &mut tree_sw_sym,
        &BootstrapInput {
            manifest_json: &manifest,
            archive: &good_archive,
            helper: Some(helper),
        },
        &pins,
    )
    .unwrap_err();
    assert!(err.to_string().contains("not a real directory"));
}

#[test]
fn stage_bootstrap_from_dir_handles_legacy_package_and_real_dir() {
    let tmp = tempfile::tempdir().unwrap();
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(
        stage_bootstrap_from_dir(&mut tree, tmp.path(), &pins)
            .unwrap()
            .is_none()
    );
    assert!(tree.lookup("Switch").is_none());

    let helper = b"helper";
    let (manifest, archive) = make_package_bytes(&[], helper, None);
    std::fs::create_dir_all(tmp.path().join("bootstrap")).unwrap();
    std::fs::create_dir_all(tmp.path().join("overlay/usr/libexec")).unwrap();
    std::fs::write(tmp.path().join("bootstrap").join(MANIFEST), manifest).unwrap();
    std::fs::write(tmp.path().join("bootstrap").join(ARCHIVE), archive).unwrap();
    std::fs::write(tmp.path().join("overlay/usr/libexec").join(HELPER), helper).unwrap();

    let report = stage_bootstrap_from_dir(&mut tree, tmp.path(), &pins).unwrap().unwrap();
    assert_eq!(report.build, "21H461");
    assert!(tree.lookup("Switch/bootstrap/21H461/root/usr/bin/tool").is_some());
}
