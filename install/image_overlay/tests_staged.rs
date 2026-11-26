// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use plist::{Dictionary, Value};
use switch_apfs_format::fs_records::SYMLINK_XATTR_NAME;
use switch_apfs_write::{Attrs, Content, Kind, Tree, XattrValue};

use crate::tests_cms::wrap_cms_der;
use crate::tests_overlay::pin_json;
use crate::{
    INSTALLER_APP, INSTALLER_IDENTIFIER, MOBILE_GID, MOBILE_UID, PLACEHOLDERS, SEEDS, SIGNATURES, STAGED, STATES,
    TreeSource, place_overlay, stage_apps, stage_apps_from_source,
};

fn plist_bytes(entries: &[(&str, Value)]) -> Vec<u8> {
    let mut dict = Dictionary::new();
    for (k, v) in entries {
        dict.insert((*k).to_owned(), v.clone());
    }
    let mut out = Vec::new();
    Value::Dictionary(dict).to_writer_xml(&mut out).unwrap();
    out
}

fn app_info_plist(id: &str, exec: &str, version: &str) -> Vec<u8> {
    plist_bytes(&[
        ("CFBundleIdentifier", Value::String(id.into())),
        ("CFBundleExecutable", Value::String(exec.into())),
        ("CFBundleVersion", Value::String(version.into())),
    ])
}

fn add_path_file(tree: &mut Tree, path: &str, bytes: &[u8], attrs: Attrs) {
    let mut current = tree.root();
    let parts: Vec<&str> = path.split('/').collect();
    for dir in &parts[..parts.len() - 1] {
        current = match tree.child(current, dir) {
            Some(existing) => existing,
            None => tree.add_dir(current, dir, Attrs::new(0, 0, 0o755)).unwrap(),
        };
    }
    let stream = if bytes.is_empty() {
        None
    } else {
        Some(tree.add_bytes(bytes))
    };
    tree.add_file(current, parts[parts.len() - 1], attrs, stream).unwrap();
}

struct SeedFixture {
    overlay: TreeSource,
    pin: Vec<u8>,
}

impl SeedFixture {
    fn new(version: &str) -> Self {
        Self::with_app(INSTALLER_APP, INSTALLER_IDENTIFIER, "SwitchInstaller", version)
    }

    fn with_app(app_name: &str, id: &str, exec: &str, version: &str) -> Self {
        let info = app_info_plist(id, exec, version);
        let exec_bytes = b"signed executable".as_slice();
        let res_bytes = b"signed resources".as_slice();

        let mut overlay = TreeSource::new();
        let app = format!("usr/share/Switch/Applications/{app_name}");
        overlay
            .add_file(&format!("{app}/Info.plist"), info.clone(), 0o444)
            .unwrap();
        overlay.add_file(&format!("{app}/{exec}"), exec_bytes, 0o555).unwrap();
        overlay
            .add_file(&format!("{app}/_CodeSignature/CodeResources"), res_bytes, 0o444)
            .unwrap();
        overlay
            .add_file(
                &format!("System/Library/AppPlaceholders/{app_name}/Info.plist"),
                info.clone(),
                0o444,
            )
            .unwrap();
        overlay
            .add_file(
                &format!("System/Library/AppSignatures/{id}"),
                &b"detached signature"[..],
                0o444,
            )
            .unwrap();

        let mut files = BTreeMap::new();
        files.insert("Info.plist", info.as_slice());
        files.insert(exec, exec_bytes);
        files.insert("_CodeSignature/CodeResources", res_bytes);
        let pin = pin_json(&files, version);
        Self { overlay, pin }
    }
}

fn read_bytes(tree: &Tree, path: &str) -> Vec<u8> {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    match tree.inode(inode).data {
        None => Vec::new(),
        Some(s) => match &tree.stream(s).content {
            Content::Bytes(b) => b.to_vec(),
            other => panic!("unexpected {other:?}"),
        },
    }
}

#[test]
fn first_boot_preserves_signature_modes_links_and_mobile_ownership() {
    let mut fixture = SeedFixture::with_app("Demo.app", "com.example.demo", "Demo", "8");
    fixture
        .overlay
        .add_symlink("usr/share/Switch/Applications/Demo.app/linked", "_CodeSignature", 0o777)
        .unwrap();

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    let report = stage_apps(&mut tree, None).unwrap();

    assert_eq!(report.staged.len(), 1);
    assert_eq!(report.staged[0].bundle_identifier, "com.example.demo");
    assert_eq!(report.staged[0].bundle_version, "8");

    let parent = tree.inode(tree.lookup(STAGED).unwrap());
    assert_eq!(
        (parent.attrs.uid, parent.attrs.gid, parent.attrs.mode),
        (501, 501, 0o755)
    );

    let exec = tree.inode(tree.lookup("staged_system_apps/Demo.app/Demo").unwrap());
    assert_eq!((exec.attrs.uid, exec.attrs.gid, exec.attrs.mode), (501, 501, 0o755));
    assert_eq!(
        read_bytes(&tree, "staged_system_apps/Demo.app/Demo"),
        b"signed executable"
    );
    assert_eq!(
        read_bytes(&tree, "staged_system_apps/Demo.app/_CodeSignature/CodeResources"),
        b"signed resources"
    );

    let linked = tree.inode(tree.lookup("staged_system_apps/Demo.app/linked").unwrap());
    assert_eq!(linked.kind, Kind::Symlink);
    assert_eq!((linked.attrs.uid, linked.attrs.gid), (MOBILE_UID, MOBILE_GID));
    let x = linked.xattrs.iter().find(|x| x.name == SYMLINK_XATTR_NAME).unwrap();
    let XattrValue::Embedded(bytes) = &x.value else {
        panic!("expected embedded symlink");
    };
    assert_eq!(bytes, b"_CodeSignature\0");
}

#[test]
fn pinned_installer_seed_verifies_and_refuses_tampering() {
    let fixture = SeedFixture::new("8");
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();

    assert!(stage_apps(&mut tree, None).is_err());
    assert!(tree.lookup(STAGED).is_none());

    let report = stage_apps(&mut tree, Some(&fixture.pin)).unwrap();
    assert_eq!(report.staged.len(), 1);
    assert_eq!(
        read_bytes(&tree, "staged_system_apps/SwitchInstaller.app/SwitchInstaller"),
        b"signed executable"
    );

    let mut bad_tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut bad_tree, &fixture.overlay, None).unwrap();
    let exec = bad_tree
        .lookup(&format!("{SEEDS}/{INSTALLER_APP}/SwitchInstaller"))
        .unwrap();
    let new_stream = bad_tree.add_bytes(&b"other code with the same bundle identifier"[..]);
    bad_tree.replace_content(exec, Some(new_stream)).unwrap();

    let err = stage_apps(&mut bad_tree, Some(&fixture.pin)).unwrap_err();
    assert!(err.to_string().contains("differs from the image input"));
    assert!(bad_tree.lookup(STAGED).is_none());
}

#[test]
fn embedded_profile_valid_cms_accepted_and_unsigned_refused() {
    let valid_plist = plist_bytes(&[("Entitlements", Value::Dictionary(Dictionary::new()))]);
    let valid_cms = wrap_cms_der(&valid_plist);

    let mut fixture = SeedFixture::with_app("Signed.app", "com.example.signed", "Signed", "8");
    fixture
        .overlay
        .add_file(
            "usr/share/Switch/Applications/Signed.app/embedded.mobileprovision",
            valid_cms,
            0o444,
        )
        .unwrap();
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    assert_eq!(stage_apps(&mut tree, None).unwrap().staged.len(), 1);

    let mut bad = SeedFixture::with_app("Unsigned.app", "com.example.unsigned", "Unsigned", "8");
    bad.overlay
        .add_file(
            "usr/share/Switch/Applications/Unsigned.app/embedded.mobileprovision",
            valid_plist,
            0o444,
        )
        .unwrap();
    let mut bad_tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut bad_tree, &bad.overlay, None).unwrap();
    let err = stage_apps(&mut bad_tree, None).unwrap_err();
    assert!(err.to_string().contains("not a decodable CMS profile"));
    assert!(bad_tree.lookup(STAGED).is_none());
}

#[test]
fn current_or_newer_install_and_deleted_app_are_skipped_while_older_and_placeholder_stage() {
    let fixture = SeedFixture::with_app("Demo.app", "com.example.demo", "Demo", "8");

    for version in ["8", "8.0", "9"] {
        let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
        place_overlay(&mut tree, &fixture.overlay, None).unwrap();
        add_path_file(
            &mut tree,
            "containers/Bundle/Application/c1/Demo.app/Info.plist",
            &app_info_plist("com.example.demo", "Demo", version),
            Attrs::new(501, 501, 0o644),
        );
        add_path_file(
            &mut tree,
            "containers/Bundle/Application/c1/Demo.app/Demo",
            b"installed",
            Attrs::new(501, 501, 0o755),
        );
        assert!(stage_apps(&mut tree, None).unwrap().staged.is_empty());
        assert!(tree.lookup("staged_system_apps/Demo.app").is_none());
    }

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    add_path_file(
        &mut tree,
        "containers/Bundle/Application/c1/Demo.app/Info.plist",
        &app_info_plist("com.example.demo", "Demo", "7"),
        Attrs::new(501, 501, 0o644),
    );
    add_path_file(
        &mut tree,
        "containers/Bundle/Application/c1/Demo.app/Demo",
        b"old",
        Attrs::new(501, 501, 0o755),
    );
    assert_eq!(stage_apps(&mut tree, None).unwrap().staged.len(), 1);

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    add_path_file(
        &mut tree,
        "containers/Bundle/Application/c1/Demo.app/Info.plist",
        &app_info_plist("com.example.demo", "Demo", "8"),
        Attrs::new(501, 501, 0o644),
    );
    assert_eq!(stage_apps(&mut tree, None).unwrap().staged.len(), 1);

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    add_path_file(
        &mut tree,
        STATES,
        &plist_bytes(&[("com.example.demo", Value::Integer(2.into()))]),
        Attrs::new(0, 0, 0o644),
    );
    assert!(stage_apps(&mut tree, None).unwrap().staged.is_empty());

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &fixture.overlay, None).unwrap();
    add_path_file(&mut tree, STATES, b"broken state", Attrs::new(0, 0, 0o644));
    assert!(stage_apps(&mut tree, None).is_err());
    assert!(tree.lookup(STAGED).is_none());
}

#[test]
fn missing_placeholder_signature_or_bad_version_refuses_staging() {
    let mut overlay = TreeSource::new();
    let info = app_info_plist("com.example.demo", "Demo", "8");
    overlay
        .add_file("usr/share/Switch/Applications/Demo.app/Info.plist", info.clone(), 0o444)
        .unwrap();
    overlay
        .add_file("usr/share/Switch/Applications/Demo.app/Demo", &b"bin"[..], 0o555)
        .unwrap();
    overlay
        .add_file("System/Library/AppSignatures/com.example.demo", &b"sig"[..], 0o444)
        .unwrap();
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree, &overlay, None).unwrap();
    assert!(stage_apps(&mut tree, None).is_err());

    let mut overlay2 = TreeSource::new();
    overlay2
        .add_file("usr/share/Switch/Applications/Demo.app/Info.plist", info.clone(), 0o444)
        .unwrap();
    overlay2
        .add_file("usr/share/Switch/Applications/Demo.app/Demo", &b"bin"[..], 0o555)
        .unwrap();
    overlay2
        .add_file("System/Library/AppPlaceholders/Demo.app/Info.plist", info, 0o444)
        .unwrap();
    let mut tree2 = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree2, &overlay2, None).unwrap();
    assert!(stage_apps(&mut tree2, None).is_err());

    let bad_ver = SeedFixture::with_app("Demo.app", "com.example.demo", "Demo", "8.0-beta");
    let mut tree3 = Tree::new(Attrs::new(0, 0, 0o755));
    place_overlay(&mut tree3, &bad_ver.overlay, None).unwrap();
    assert!(stage_apps(&mut tree3, None).is_err());

    assert_eq!(PLACEHOLDERS, "Switch/overlay/System/Library/AppPlaceholders");
    assert_eq!(SIGNATURES, "Switch/overlay/System/Library/AppSignatures");
}

#[test]
fn no_seed_leaves_apples_staged_apps_alone_and_source_api_works() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let staged = tree.add_dir(tree.root(), STAGED, Attrs::new(501, 501, 0o755)).unwrap();
    tree.add_dir(staged, "Maps.app", Attrs::new(501, 501, 0o755)).unwrap();

    assert!(stage_apps(&mut tree, None).unwrap().staged.is_empty());
    assert!(tree.lookup("staged_system_apps/Maps.app").is_some());

    let fixture = SeedFixture::new("10");
    let report = stage_apps_from_source(&mut tree, &fixture.overlay, Some(&fixture.pin)).unwrap();
    assert_eq!(report.staged.len(), 1);
    assert!(tree.lookup("staged_system_apps/Maps.app").is_some());
    assert!(
        tree.lookup("staged_system_apps/SwitchInstaller.app/SwitchInstaller")
            .is_some()
    );
}
