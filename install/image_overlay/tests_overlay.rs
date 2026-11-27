// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io::Cursor;

use sha2::{Digest, Sha256};
use switch_apfs_format::fs_records::SYMLINK_XATTR_NAME;
use switch_apfs_write::{Attrs, Content, Kind, Tree, XattrValue};

use crate::{INSTALLER_APP, INSTALLER_IDENTIFIER, TreeSource, merge_tree, place_overlay, tree_digest};

pub(crate) fn pin_json(files: &BTreeMap<&str, &[u8]>, version: &str) -> Vec<u8> {
    let mut map = serde_json::Map::new();
    for (k, v) in files {
        map.insert(
            (*k).to_owned(),
            serde_json::Value::String(hex::encode(Sha256::digest(v))),
        );
    }
    serde_json::to_vec(&serde_json::json!({
        "schema": 1,
        "identifier": INSTALLER_IDENTIFIER,
        "version": version,
        "files": map,
    }))
    .unwrap()
}

fn build_tar(entries: &[(&str, tar::EntryType, u32, &[u8], Option<&str>)]) -> Vec<u8> {
    let mut builder = tar::Builder::new(Vec::new());
    for &(name, kind, mode, data, link) in entries {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(kind);
        header.set_mode(mode);
        header.set_size(if kind.is_file() { data.len() as u64 } else { 0 });
        header.set_uid(0);
        header.set_gid(0);
        header.set_mtime(0);
        if let Some(target) = link {
            builder.append_link(&mut header, name, target).unwrap();
        } else {
            builder.append_data(&mut header, name, data).unwrap();
        }
    }
    builder.into_inner().unwrap()
}

fn read_bytes(tree: &Tree, path: &str) -> Vec<u8> {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    let node = tree.inode(inode);
    assert_eq!(node.kind, Kind::File);
    match node.data {
        None => Vec::new(),
        Some(s) => match &tree.stream(s).content {
            Content::Bytes(b) => b.to_vec(),
            other => panic!("unexpected stream {other:?}"),
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
fn place_overlay_sets_modes_owners_skips_apple_double_and_preserves_symlinks() {
    let tar = build_tar(&[
        ("overlay/", tar::EntryType::Directory, 0o555, b"", None),
        ("overlay/usr/libexec/tool", tar::EntryType::Regular, 0o555, b"bin", None),
        ("overlay/etc/conf", tar::EntryType::Regular, 0o444, b"cfg", None),
        (
            "overlay/etc/._conf",
            tar::EntryType::Regular,
            0o644,
            b"appledouble",
            None,
        ),
        ("overlay/._root", tar::EntryType::Regular, 0o644, b"appledouble", None),
        (
            "overlay/usr/bin/link",
            tar::EntryType::Symlink,
            0o777,
            b"",
            Some("../libexec/tool"),
        ),
        ("overlay/empty", tar::EntryType::Regular, 0o400, b"", None),
        ("overlay/sub", tar::EntryType::Directory, 0o500, b"", None),
    ]);
    let source = TreeSource::from_tar_prefix(Cursor::new(tar), "overlay").unwrap();
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let report = place_overlay(&mut tree, &source, None).unwrap();

    assert_eq!(report.files, 3);
    assert_eq!(report.symlinks, 1);
    assert_eq!(report.skipped_apple_double, 2);
    assert!(!report.builtin_pin_verified);

    let tool = tree.inode(tree.lookup("Switch/overlay/usr/libexec/tool").unwrap());
    assert_eq!((tool.attrs.uid, tool.attrs.gid, tool.attrs.mode), (0, 0, 0o755));
    assert_eq!(read_bytes(&tree, "Switch/overlay/usr/libexec/tool"), b"bin");

    let conf = tree.inode(tree.lookup("Switch/overlay/etc/conf").unwrap());
    assert_eq!((conf.attrs.uid, conf.attrs.gid, conf.attrs.mode), (0, 0, 0o644));

    let empty = tree.inode(tree.lookup("Switch/overlay/empty").unwrap());
    assert_eq!(empty.attrs.mode, 0o600);
    assert!(empty.data.is_none());

    let sub = tree.inode(tree.lookup("Switch/overlay/sub").unwrap());
    assert_eq!((sub.attrs.uid, sub.attrs.gid, sub.attrs.mode), (0, 0, 0o755));

    assert!(tree.lookup("Switch/overlay/etc/._conf").is_none());
    assert!(tree.lookup("Switch/overlay/._root").is_none());
    assert_eq!(read_symlink(&tree, "Switch/overlay/usr/bin/link"), "../libexec/tool");
}

#[test]
fn place_overlay_verifies_builtin_installer_pin() {
    let mut source = TreeSource::new();
    let app = format!("Applications/{INSTALLER_APP}");
    source
        .add_file(&format!("{app}/Info.plist"), &b"plist"[..], 0o444)
        .unwrap();
    source
        .add_file(&format!("{app}/SwitchInstaller"), &b"exec"[..], 0o555)
        .unwrap();
    source
        .add_file(&format!("{app}/_CodeSignature/CodeResources"), &b"seal"[..], 0o444)
        .unwrap();

    let mut files = BTreeMap::new();
    files.insert("Info.plist", &b"plist"[..]);
    files.insert("SwitchInstaller", &b"exec"[..]);
    files.insert("_CodeSignature/CodeResources", &b"seal"[..]);
    let pin = pin_json(&files, "10");

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(place_overlay(&mut tree, &source, None).is_err());
    assert!(tree.lookup("Switch").is_none());

    let report = place_overlay(&mut tree, &source, Some(&pin)).unwrap();
    assert!(report.builtin_pin_verified);
    assert_eq!(
        read_bytes(&tree, &format!("Switch/overlay/{app}/SwitchInstaller")),
        b"exec"
    );

    let mut tampered = source.clone();
    let mut bad = TreeSource::new();
    bad.add_file(&format!("{app}/Info.plist"), &b"plist"[..], 0o444)
        .unwrap();
    bad.add_file(&format!("{app}/SwitchInstaller"), &b"tampered"[..], 0o555)
        .unwrap();
    bad.add_file(&format!("{app}/_CodeSignature/CodeResources"), &b"seal"[..], 0o444)
        .unwrap();
    let mut tree2 = Tree::new(Attrs::new(0, 0, 0o755));
    let err = place_overlay(&mut tree2, &bad, Some(&pin)).unwrap_err();
    assert!(err.to_string().contains("differs"));
    assert!(tree2.lookup("Switch").is_none());

    tampered.add_file(&format!("{app}/extra"), &b"x"[..], 0o644).unwrap();
    let mut tree3 = Tree::new(Attrs::new(0, 0, 0o755));
    let err = place_overlay(&mut tree3, &tampered, Some(&pin)).unwrap_err();
    assert!(err.to_string().contains("differs"));

    let mut linked = TreeSource::new();
    linked
        .add_file(&format!("{app}/Info.plist"), &b"plist"[..], 0o444)
        .unwrap();
    linked
        .add_symlink(&format!("{app}/SwitchInstaller"), "other", 0o777)
        .unwrap();
    linked
        .add_file(&format!("{app}/_CodeSignature/CodeResources"), &b"seal"[..], 0o444)
        .unwrap();
    let mut tree4 = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(place_overlay(&mut tree4, &linked, Some(&pin)).is_err());

    let mut extra_dir = source.clone();
    extra_dir.add_dir(&format!("{app}/EmptyDir"), 0o755).unwrap();
    let mut tree5 = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(place_overlay(&mut tree5, &extra_dir, Some(&pin)).is_err());

    let empty = TreeSource::new();
    let mut tree6 = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(place_overlay(&mut tree6, &empty, Some(&pin)).is_err());
}

#[test]
fn from_dir_and_tree_digest_match_expected_format() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(dir.path().join("usr/bin")).unwrap();
    std::fs::write(dir.path().join("usr/bin/a"), b"hello").unwrap();
    std::fs::write(dir.path().join("usr/bin/._a"), b"ignored").unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir.path().join("usr/bin/a"), std::fs::Permissions::from_mode(0o555)).unwrap();
    }

    let source = TreeSource::from_dir(dir.path()).unwrap();
    let digest = tree_digest(&source);

    let file_hash = hex::encode(Sha256::digest(b"hello"));
    #[cfg(unix)]
    let mode = "755";
    #[cfg(not(unix))]
    let mode = "644";
    let expected = hex::encode(Sha256::digest(format!(
        "d usr\nd usr/bin\nf usr/bin/a {mode} {file_hash}\n"
    )));
    assert_eq!(digest, expected);
}

#[test]
fn merge_tree_reuses_dirs_replaces_files_and_sets_root_modes() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let mobile = tree
        .add_dir(tree.root(), "mobile", Attrs::new(501, 501, 0o700))
        .unwrap();
    let old_stream = tree.add_bytes(&b"old"[..]);
    tree.add_file(mobile, "cache.bin", Attrs::new(501, 501, 0o600), Some(old_stream))
        .unwrap();

    let mut seed = TreeSource::new();
    seed.add_dir("mobile", 0o700).unwrap();
    seed.add_dir("mobile/shaders", 0o700).unwrap();
    seed.add_file("mobile/cache.bin", &b"new"[..], 0o755).unwrap();
    seed.add_file("mobile/shaders/s0.bin", &b"shader"[..], 0o555).unwrap();

    let report = merge_tree(&mut tree, &seed).unwrap();
    assert_eq!(report.dirs, 1);
    assert_eq!(report.files, 2);
    assert_eq!(report.replaced_files, 1);

    let mobile_node = tree.inode(tree.lookup("mobile").unwrap());
    assert_eq!(
        (mobile_node.attrs.uid, mobile_node.attrs.gid, mobile_node.attrs.mode),
        (0, 0, 0o755)
    );
    let replaced = tree.inode(tree.lookup("mobile/cache.bin").unwrap());
    assert_eq!(
        (replaced.attrs.uid, replaced.attrs.gid, replaced.attrs.mode),
        (0, 0, 0o644)
    );
    assert_eq!(read_bytes(&tree, "mobile/cache.bin"), b"new");

    let added = tree.inode(tree.lookup("mobile/shaders/s0.bin").unwrap());
    assert_eq!((added.attrs.uid, added.attrs.gid, added.attrs.mode), (0, 0, 0o644));
    assert_eq!(read_bytes(&tree, "mobile/shaders/s0.bin"), b"shader");
}

#[test]
fn merge_tree_refuses_symlinks_and_type_conflicts_atomically() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let s = tree.add_bytes(&b"keep"[..]);
    tree.add_file(tree.root(), "conflict", Attrs::new(0, 0, 0o644), Some(s))
        .unwrap();

    let mut bad = TreeSource::new();
    bad.add_file("first", &b"1"[..], 0o644).unwrap();
    bad.add_dir("conflict", 0o755).unwrap();
    assert!(merge_tree(&mut tree, &bad).is_err());
    assert!(tree.lookup("first").is_none());

    let mut with_link = TreeSource::new();
    with_link.add_symlink("ln", "target", 0o777).unwrap();
    assert!(merge_tree(&mut tree, &with_link).is_err());
}

#[test]
fn from_tar_refuses_special_files_and_unsafe_paths() {
    let fifo = build_tar(&[("pipe", tar::EntryType::Fifo, 0o644, b"", None)]);
    assert!(TreeSource::from_tar(Cursor::new(fifo)).is_err());

    let mut escape = TreeSource::new();
    assert!(escape.add_file("../outside", &b"x"[..], 0o644).is_err());
    assert!(escape.add_file("/abs", &b"x"[..], 0o644).is_err());
    assert!(escape.add_file("a//b", &b"x"[..], 0o644).is_err());
}
