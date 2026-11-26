// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::sync::Arc;

use plist::Value as PlistValue;
use sha2::{Digest, Sha256};
use tempfile::TempDir;

use crate::{Error, FileContent, GpuStage, ParsedManifest, REQUIRED_PLUGIN_PATH, TREE_MANIFEST, check_path};

fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

fn minimal_tree_manifest(kext: Option<&str>, plugin: &[u8], extra_lines: &[&str]) -> String {
    let mut lines = vec!["# gpu stage 2 tree".to_owned()];
    if let Some(k) = kext {
        lines.push(format!("# kext {k}"));
    }
    lines.push(format!(
        "file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}",
        sha256_hex(plugin),
        plugin.len()
    ));
    for extra in extra_lines {
        lines.push((*extra).to_owned());
    }
    lines.join("\n") + "\n"
}

fn build_tar(files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut builder = tar::Builder::new(Vec::new());
    for &(name, data) in files {
        let mut header = tar::Header::new_gnu();
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        header.set_uid(0);
        header.set_gid(0);
        header.set_mtime(0);
        header.set_cksum();
        builder.append_data(&mut header, name, data).unwrap();
    }
    builder.into_inner().unwrap()
}

#[test]
fn parse_tree_directory_and_file_with_kext_and_prefs() {
    let tmp = TempDir::new().unwrap();
    let plugin = b"macho-plugin-binary";
    let info = b"<plist/>";
    let pack = b"pipeline-pack-bytes";
    let kext = "a".repeat(64);

    let info_path = "ngpu/addons/TegraGM20BMetal.bundle/Info.plist";
    let pack_path = "ngpu/addons/TegraGM20BMetal.pack/pipelines.bin";

    let manifest = minimal_tree_manifest(
        Some(&kext),
        plugin,
        &[
            &format!(
                "file\t{info_path}\t{}\t{}\t644\t{info_path}",
                sha256_hex(info).to_uppercase(),
                info.len()
            ),
            &format!(
                "file\t{pack_path}\t{}\t{}\t644\t{pack_path}",
                sha256_hex(pack),
                pack.len()
            ),
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableDeepFramebuffer\tfalse",
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tMaxFPS\t60",
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tScale\t1.5",
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tLabel\t\"tegra\"",
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tList\t[1, \"two\", true]",
            "pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tNested\t{\"k\": -7}",
            "pref\troot/Library/Preferences/com.apple.backboardd.plist\t0\tCAEnableDeepFramebuffer\tfalse",
        ],
    );
    fs::write(tmp.path().join(TREE_MANIFEST), &manifest).unwrap();

    let from_dir = GpuStage::from_path(tmp.path()).unwrap();
    let from_file = GpuStage::from_path(&tmp.path().join(TREE_MANIFEST)).unwrap();

    assert_eq!(from_dir.kext.as_deref(), Some(kext.as_str()));
    assert_eq!(from_dir.files.len(), 3);
    assert_eq!(from_dir.prefs.len(), 2);
    assert_eq!(from_dir.whole_digest(), from_file.whole_digest());
    assert_eq!(from_dir.icon_build(), from_file.icon_build());

    let expected_body = format!(
        "{info_path}\t{}\t{}\t644\n{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\n{pack_path}\t{}\t{}\t644\n",
        sha256_hex(info),
        info.len(),
        sha256_hex(plugin),
        plugin.len(),
        sha256_hex(pack),
        pack.len(),
    );
    let expected_whole = sha256_hex(expected_body.as_bytes());
    let expected_icon = sha256_hex(format!("{expected_whole}\n{kext}").as_bytes());

    assert_eq!(from_dir.tsv_body(), expected_body);
    assert_eq!(from_dir.whole_digest(), expected_whole);
    assert_eq!(from_dir.icon_build(), expected_icon);
    assert_eq!(from_dir.stage_tsv(), format!("# {expected_whole}\n{expected_body}"));

    let mobile = &from_dir.prefs["mobile/Library/Preferences/com.apple.backboardd.plist"];
    assert_eq!(mobile.owner_uid, 501);
    assert_eq!(mobile.values["CAEnableDeepFramebuffer"], PlistValue::Boolean(false));
    assert_eq!(mobile.values["MaxFPS"].as_signed_integer(), Some(60));
    assert_eq!(mobile.values["Scale"].as_real(), Some(1.5));
    assert_eq!(mobile.values["Label"].as_string(), Some("tegra"));
    let list = mobile.values["List"].as_array().unwrap();
    assert_eq!(list.len(), 3);
    assert_eq!(list[0].as_signed_integer(), Some(1));
    assert_eq!(list[1].as_string(), Some("two"));
    assert_eq!(list[2].as_boolean(), Some(true));
    let nested = mobile.values["Nested"].as_dictionary().unwrap();
    assert_eq!(nested.get("k").and_then(PlistValue::as_signed_integer), Some(-7));
}

#[test]
fn icon_build_without_kext_equals_whole_digest() {
    let plugin = b"plugin-only";
    let manifest = minimal_tree_manifest(None, plugin, &[]);
    let stage = GpuStage::parse_manifest(&manifest, Path::new("/tmp/stage/gpu.stage")).unwrap();
    assert!(stage.kext.is_none());
    assert_eq!(stage.icon_build(), stage.whole_digest());
}

#[test]
fn parse_runfiles_manifest_resolves_root_and_refuses_mismatch() {
    let plugin = b"runfiles-plugin";
    let manifest = format!(
        "# gpu stage 2 runfiles _main/gpu/bundle.stage\n\
         file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t_main/drivers/TegraGM20BMetal/TegraGM20BMetal\n",
        sha256_hex(plugin),
        plugin.len(),
    );

    let stage = GpuStage::parse_manifest(&manifest, Path::new("/rf/runfiles/_main/gpu/bundle.stage")).unwrap();
    let rec = &stage.files[REQUIRED_PLUGIN_PATH];
    match &rec.content {
        FileContent::Path(p) => assert_eq!(
            p,
            Path::new("/rf/runfiles/_main/drivers/TegraGM20BMetal/TegraGM20BMetal")
        ),
        FileContent::Bytes(_) => panic!("expected Path content"),
    }

    assert!(GpuStage::parse_manifest(&manifest, Path::new("/rf/wrong/gpu/bundle.stage")).is_err());
    assert!(GpuStage::parse_manifest(&manifest, Path::new("/rf/x_main/gpu/bundle.stage")).is_err());
    assert!(GpuStage::parse_manifest(&manifest, Path::new("_main/gpu/bundle.stage")).is_err());
}

#[test]
fn from_tar_supports_top_level_and_prefixed_stage_trees() {
    let plugin = b"tar-plugin-bytes";
    let info = b"tar-info";
    let info_path = "ngpu/addons/TegraGM20BMetal.bundle/Info.plist";
    let manifest = minimal_tree_manifest(
        Some("deadbeef"),
        plugin,
        &[&format!(
            "file\t{info_path}\t{}\t{}\t644\t{info_path}",
            sha256_hex(info),
            info.len()
        )],
    );

    let top_tar = build_tar(&[
        (TREE_MANIFEST, manifest.as_bytes()),
        (REQUIRED_PLUGIN_PATH, plugin),
        (info_path, info),
    ]);
    let stage_top = GpuStage::from_tar(top_tar.as_slice()).unwrap();
    assert_eq!(stage_top.files.len(), 2);
    assert_eq!(stage_top.kext.as_deref(), Some("deadbeef"));

    let prefixed_tar = build_tar(&[
        (&format!("gpu/{TREE_MANIFEST}"), manifest.as_bytes()),
        (&format!("gpu/{REQUIRED_PLUGIN_PATH}"), plugin),
        (&format!("gpu/{info_path}"), info),
    ]);
    let stage_prefixed = GpuStage::from_tar(prefixed_tar.as_slice()).unwrap();
    assert_eq!(stage_prefixed.whole_digest(), stage_top.whole_digest());
    assert_eq!(stage_prefixed.icon_build(), stage_top.icon_build());

    let missing_manifest_tar = build_tar(&[(REQUIRED_PLUGIN_PATH, plugin)]);
    assert!(matches!(
        GpuStage::from_tar(missing_manifest_tar.as_slice()),
        Err(Error::MissingTreeManifest(_))
    ));

    let missing_file_tar = build_tar(&[(TREE_MANIFEST, manifest.as_bytes()), (REQUIRED_PLUGIN_PATH, plugin)]);
    assert!(matches!(
        GpuStage::from_tar(missing_file_tar.as_slice()),
        Err(Error::MissingSourceFile(_))
    ));
}

#[test]
fn parse_manifest_refuses_all_invalid_inputs() {
    let plugin = b"abc";
    let sha = sha256_hex(plugin);

    for bad_header in [
        "",
        "# gpu stage 1 tree\n",
        "# gpu stage 2\n",
        "# gpu stage 2 tree extra\n",
        "# gpu stage 2 runfiles\n",
        "# gpu stage 2 runfiles \n",
        "# gpu stage 2 unknown foo\n",
    ] {
        assert!(ParsedManifest::parse(bad_header).is_err(), "accepted {bad_header:?}");
    }

    let empty_dir = TempDir::new().unwrap();
    assert!(matches!(
        GpuStage::from_path(empty_dir.path()),
        Err(Error::MissingTreeManifest(_))
    ));

    for bad in [
        "", "/", "/ngpu/a", "ngpu/a/", "ngpu//a", ".", "..", "a/./b", "a/../b", "a\0b",
    ] {
        assert!(
            matches!(check_path(bad), Err(Error::InvalidPath(_))),
            "accepted {bad:?}"
        );
    }
    let bad_file_path = format!("# gpu stage 2 tree\nfile\t../evil\t{sha}\t3\t644\t../evil\n");
    assert!(matches!(
        ParsedManifest::parse(&bad_file_path),
        Err(Error::InvalidPath(_))
    ));
    let abs_file_path =
        format!("# gpu stage 2 tree\nfile\t/{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t755\t{REQUIRED_PLUGIN_PATH}\n");
    assert!(matches!(
        ParsedManifest::parse(&abs_file_path),
        Err(Error::InvalidPath(_))
    ));
    let bad_pref_path = minimal_tree_manifest(None, plugin, &["pref\t../bad.plist\t0\tK\tfalse"]);
    assert!(matches!(
        ParsedManifest::parse(&bad_pref_path),
        Err(Error::InvalidPath(_))
    ));

    let no_plugin = format!("# gpu stage 2 tree\nfile\tngpu/addons/other\t{sha}\t3\t644\tngpu/addons/other\n");
    assert!(matches!(ParsedManifest::parse(&no_plugin), Err(Error::MissingPlugin)));

    let dup = minimal_tree_manifest(
        None,
        plugin,
        &[&format!(
            "file\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t755\t{REQUIRED_PLUGIN_PATH}"
        )],
    );
    assert!(matches!(ParsedManifest::parse(&dup), Err(Error::DuplicateFile(_))));

    let conflict_owner = minimal_tree_manifest(
        None,
        plugin,
        &[
            "pref\tmobile/Library/Preferences/a.plist\t501\tA\tfalse",
            "pref\tmobile/Library/Preferences/a.plist\t0\tB\tfalse",
        ],
    );
    assert!(matches!(
        ParsedManifest::parse(&conflict_owner),
        Err(Error::ConflictingPrefOwner { .. })
    ));

    let conflict_kext = format!(
        "# gpu stage 2 tree\n# kext aaa\n# kext bbb\nfile\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t755\t{REQUIRED_PLUGIN_PATH}\n"
    );
    assert!(ParsedManifest::parse(&conflict_kext).is_err());
    let empty_kext =
        format!("# gpu stage 2 tree\n# kext \nfile\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t755\t{REQUIRED_PLUGIN_PATH}\n");
    assert!(ParsedManifest::parse(&empty_kext).is_err());

    let same_kext = format!(
        "# gpu stage 2 tree\n# kext aaa\n# kext aaa\nfile\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t755\t{REQUIRED_PLUGIN_PATH}\n"
    );
    assert_eq!(ParsedManifest::parse(&same_kext).unwrap().kext.as_deref(), Some("aaa"));

    let unknown_kind = minimal_tree_manifest(None, plugin, &["link\ta\tb"]);
    assert!(ParsedManifest::parse(&unknown_kind).is_err());
    let short_file = minimal_tree_manifest(None, plugin, &["file\tngpu/a\t00\t1\t644"]);
    assert!(ParsedManifest::parse(&short_file).is_err());
    let extra_pref = minimal_tree_manifest(None, plugin, &["pref\ta.plist\t0\tK\tfalse\textra"]);
    assert!(ParsedManifest::parse(&extra_pref).is_err());

    let null_pref = minimal_tree_manifest(None, plugin, &["pref\ta.plist\t0\tK\tnull"]);
    assert!(matches!(ParsedManifest::parse(&null_pref), Err(Error::NullPrefValue)));
    let nested_null = minimal_tree_manifest(None, plugin, &["pref\ta.plist\t0\tK\t[1, null]"]);
    assert!(matches!(ParsedManifest::parse(&nested_null), Err(Error::NullPrefValue)));

    let bad_sha = format!("# gpu stage 2 tree\nfile\t{REQUIRED_PLUGIN_PATH}\t1234\t3\t755\t{REQUIRED_PLUGIN_PATH}\n");
    assert!(ParsedManifest::parse(&bad_sha).is_err());
    let bad_mode = format!("# gpu stage 2 tree\nfile\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t888\t{REQUIRED_PLUGIN_PATH}\n");
    assert!(ParsedManifest::parse(&bad_mode).is_err());
    let wide_mode =
        format!("# gpu stage 2 tree\nfile\t{REQUIRED_PLUGIN_PATH}\t{sha}\t3\t100755\t{REQUIRED_PLUGIN_PATH}\n");
    assert!(ParsedManifest::parse(&wide_mode).is_err());

    let mem_manifest = minimal_tree_manifest(None, plugin, &[]);
    assert!(matches!(
        GpuStage::parse_manifest_memory(&mem_manifest, &BTreeMap::<String, Arc<[u8]>>::new()),
        Err(Error::MissingSourceFile(_))
    ));
}
