// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::io::{Cursor, Read};

use flate2::read::GzDecoder;
use plist::{Dictionary, Value as PlistValue};
use sha2::{Digest, Sha256};
use switch_apfs_write::{Attrs, Content, Tree};
use tempfile::TempDir;

use crate::{
    Error, GpuStage, ICON_CACHE_RECORD_PATH, LARGE_FILE_THRESHOLD, REQUIRED_PLUGIN_PATH, STATE_PATH, TREE_MANIFEST,
    stage_gpu_from_path,
};

fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

fn read_bytes(tree: &Tree, path: &str) -> Vec<u8> {
    let inode = tree.lookup(path).unwrap_or_else(|| panic!("missing {path}"));
    match tree.inode(inode).data {
        None => Vec::new(),
        Some(s) => match &tree.stream(s).content {
            Content::Bytes(b) => b.to_vec(),
            other => panic!("{path}: expected Content::Bytes, got {other:?}"),
        },
    }
}

#[test]
fn stage_gpu_disk_large_fill_small_bytes_state_gz_icon_cache_and_plist_merge() {
    let tmp = TempDir::new().unwrap();
    let stage_dir = tmp.path().join("gpu_tree");
    fs::create_dir_all(stage_dir.join("ngpu/addons/TegraGM20BMetal.bundle")).unwrap();
    fs::create_dir_all(stage_dir.join("ngpu/addons/TegraGM20BMetal.pack")).unwrap();

    let plugin = b"#!/plugin-macho-binary";
    let empty: &[u8] = b"";
    let large = vec![0x5Au8; (LARGE_FILE_THRESHOLD as usize) + 4096];
    let kext = "0123456789abcdef".repeat(4);

    let empty_path = "ngpu/addons/TegraGM20BMetal.bundle/empty.dat";
    let large_path = "ngpu/addons/TegraGM20BMetal.pack/pipelines.bin";

    fs::write(stage_dir.join(REQUIRED_PLUGIN_PATH), plugin).unwrap();
    fs::write(stage_dir.join(empty_path), empty).unwrap();
    fs::write(stage_dir.join(large_path), &large).unwrap();

    let manifest = format!(
        "# gpu stage 2 tree\n\
         # kext {kext}\n\
         file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n\
         file\t{empty_path}\t{}\t0\t644\t{empty_path}\n\
         file\t{large_path}\t{}\t{}\t644\t{large_path}\n\
         pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableDeepFramebuffer\tfalse\n\
         pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableFramebufferCompression\tfalse\n\
         pref\tmobile/Library/Preferences/.GlobalPreferences.plist\t501\tCAEnableDeepFramebuffer\tfalse\n\
         pref\troot/Library/Preferences/com.apple.backboardd.plist\t0\tCAEnableDeepFramebuffer\tfalse\n",
        sha256_hex(plugin),
        plugin.len(),
        sha256_hex(empty),
        sha256_hex(&large),
        large.len(),
    );
    fs::write(stage_dir.join(TREE_MANIFEST), &manifest).unwrap();

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let mobile = tree
        .add_dir(tree.root(), "mobile", Attrs::new(501, 501, 0o755))
        .unwrap();
    let lib = tree.add_dir(mobile, "Library", Attrs::new(501, 501, 0o700)).unwrap();
    let prefs_dir = tree.add_dir(lib, "Preferences", Attrs::new(501, 501, 0o700)).unwrap();

    let xml_plist = b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
        <!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
        <plist version=\"1.0\"><dict>\n\
        <key>KeepMe</key><string>preserved</string>\n\
        <key>CAEnableDeepFramebuffer</key><true/>\n\
        </dict></plist>\n";
    let xml_stream = tree.add_bytes(xml_plist.to_vec());
    tree.add_file(
        prefs_dir,
        "com.apple.backboardd.plist",
        Attrs::new(501, 501, 0o600),
        Some(xml_stream),
    )
    .unwrap();

    let mut existing_global = Dictionary::new();
    existing_global.insert("AppleLocale".into(), PlistValue::String("en_US".into()));
    let mut existing_global_bin = Vec::new();
    plist::to_writer_binary(&mut existing_global_bin, &PlistValue::Dictionary(existing_global)).unwrap();
    let global_stream = tree.add_bytes(existing_global_bin);
    tree.add_file(
        prefs_dir,
        ".GlobalPreferences.plist",
        Attrs::new(0, 0, 0o600),
        Some(global_stream),
    )
    .unwrap();

    let report = stage_gpu_from_path(&mut tree, None, &stage_dir).unwrap();
    assert_eq!(report.files, 3);
    assert_eq!(report.prefs, 3);

    for dir in ["ngpu", "ngpu/addons", "Switch"] {
        let ino = tree.lookup(dir).unwrap();
        let a = &tree.inode(ino).attrs;
        assert_eq!((a.uid, a.gid, a.mode), (0, 0, 0o755), "{dir}");
    }
    assert_eq!(
        (
            tree.inode(lib).attrs.uid,
            tree.inode(lib).attrs.gid,
            tree.inode(lib).attrs.mode
        ),
        (501, 501, 0o700)
    );
    assert_eq!(
        (
            tree.inode(prefs_dir).attrs.uid,
            tree.inode(prefs_dir).attrs.gid,
            tree.inode(prefs_dir).attrs.mode
        ),
        (501, 501, 0o700)
    );

    let plugin_ino = tree.lookup(REQUIRED_PLUGIN_PATH).unwrap();
    assert_eq!(tree.inode(plugin_ino).attrs.mode, 0o755);
    assert_eq!(read_bytes(&tree, REQUIRED_PLUGIN_PATH), plugin);

    let empty_ino = tree.lookup(empty_path).unwrap();
    assert!(tree.inode(empty_ino).data.is_none());
    assert_eq!(tree.inode(empty_ino).attrs.mode, 0o644);

    let large_ino = tree.lookup(large_path).unwrap();
    let large_stream = tree.inode(large_ino).data.unwrap();
    assert_eq!(tree.stream(large_stream).size, large.len() as u64);
    assert!(matches!(tree.stream(large_stream).content, Content::Fill(_)));

    let gz_ino = tree.lookup(STATE_PATH).unwrap();
    let gz_attrs = &tree.inode(gz_ino).attrs;
    assert_eq!((gz_attrs.uid, gz_attrs.gid, gz_attrs.mode), (0, 0, 0o644));
    let gz_bytes = read_bytes(&tree, STATE_PATH);
    let mut decompressed = String::new();
    GzDecoder::new(gz_bytes.as_slice())
        .read_to_string(&mut decompressed)
        .unwrap();
    let stage = GpuStage::from_path(&stage_dir).unwrap();
    assert_eq!(decompressed, stage.stage_tsv());
    assert!(decompressed.starts_with(&format!("# {}\n", report.whole_digest)));

    let icon_ino = tree.lookup(ICON_CACHE_RECORD_PATH).unwrap();
    let icon_attrs = &tree.inode(icon_ino).attrs;
    assert_eq!((icon_attrs.uid, icon_attrs.gid, icon_attrs.mode), (0, 0, 0o644));
    assert_eq!(
        read_bytes(&tree, ICON_CACHE_RECORD_PATH),
        format!("{}\n", report.icon_build).as_bytes()
    );

    let bb_ino = tree
        .lookup("mobile/Library/Preferences/com.apple.backboardd.plist")
        .unwrap();
    let bb_attrs = &tree.inode(bb_ino).attrs;
    assert_eq!((bb_attrs.uid, bb_attrs.gid, bb_attrs.mode), (501, 501, 0o644));
    let bb_raw = read_bytes(&tree, "mobile/Library/Preferences/com.apple.backboardd.plist");
    assert!(bb_raw.starts_with(b"bplist00"));
    let bb = PlistValue::from_reader(Cursor::new(&bb_raw)).unwrap();
    let bb_dict = bb.as_dictionary().unwrap();
    assert_eq!(bb_dict.get("KeepMe").and_then(PlistValue::as_string), Some("preserved"));
    assert_eq!(
        bb_dict.get("CAEnableDeepFramebuffer").and_then(PlistValue::as_boolean),
        Some(false)
    );
    assert_eq!(
        bb_dict
            .get("CAEnableFramebufferCompression")
            .and_then(PlistValue::as_boolean),
        Some(false)
    );

    let gl_ino = tree
        .lookup("mobile/Library/Preferences/.GlobalPreferences.plist")
        .unwrap();
    let gl_attrs = &tree.inode(gl_ino).attrs;
    assert_eq!((gl_attrs.uid, gl_attrs.gid, gl_attrs.mode), (501, 501, 0o644));
    let gl_raw = read_bytes(&tree, "mobile/Library/Preferences/.GlobalPreferences.plist");
    assert!(gl_raw.starts_with(b"bplist00"));
    let gl_dict = PlistValue::from_reader(Cursor::new(&gl_raw))
        .unwrap()
        .into_dictionary()
        .unwrap();
    assert_eq!(
        gl_dict.get("AppleLocale").and_then(PlistValue::as_string),
        Some("en_US")
    );
    assert_eq!(
        gl_dict.get("CAEnableDeepFramebuffer").and_then(PlistValue::as_boolean),
        Some(false)
    );

    let rt_ino = tree
        .lookup("root/Library/Preferences/com.apple.backboardd.plist")
        .unwrap();
    let rt_attrs = &tree.inode(rt_ino).attrs;
    assert_eq!((rt_attrs.uid, rt_attrs.gid, rt_attrs.mode), (0, 0, 0o644));
    let rt_raw = read_bytes(&tree, "root/Library/Preferences/com.apple.backboardd.plist");
    assert!(rt_raw.starts_with(b"bplist00"));
}

#[test]
fn stage_gpu_refuses_size_and_sha_mismatch_without_mutating_tree() {
    let tmp = TempDir::new().unwrap();
    let plugin = b"good-plugin";
    let bad_plugin = b"bad!-plugin";
    let dir = tmp.path().join("bad_sha");
    fs::create_dir_all(dir.join("ngpu/addons/TegraGM20BMetal.bundle")).unwrap();
    fs::write(dir.join(REQUIRED_PLUGIN_PATH), bad_plugin).unwrap();

    let manifest = format!(
        "# gpu stage 2 tree\nfile\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n",
        sha256_hex(plugin),
        plugin.len(),
    );
    fs::write(dir.join(TREE_MANIFEST), &manifest).unwrap();

    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(matches!(
        stage_gpu_from_path(&mut tree, None, &dir),
        Err(Error::DigestMismatch { .. })
    ));
    assert!(tree.lookup("ngpu").is_none());

    fs::write(dir.join(REQUIRED_PLUGIN_PATH), b"short").unwrap();
    assert!(matches!(
        stage_gpu_from_path(&mut tree, None, &dir),
        Err(Error::SizeMismatch { .. })
    ));
    assert!(tree.lookup("ngpu").is_none());

    let large_dir = tmp.path().join("bad_large");
    fs::create_dir_all(large_dir.join("ngpu/addons/TegraGM20BMetal.bundle")).unwrap();
    let large_buf = vec![0x33u8; (LARGE_FILE_THRESHOLD as usize) + 16];
    fs::write(large_dir.join(REQUIRED_PLUGIN_PATH), &large_buf).unwrap();
    let wrong_sha = "ff".repeat(32);
    let large_manifest = format!(
        "# gpu stage 2 tree\nfile\t{REQUIRED_PLUGIN_PATH}\t{wrong_sha}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n",
        large_buf.len(),
    );
    fs::write(large_dir.join(TREE_MANIFEST), &large_manifest).unwrap();
    assert!(matches!(
        stage_gpu_from_path(&mut tree, None, &large_dir),
        Err(Error::DigestMismatch { .. })
    ));
    assert!(tree.lookup("ngpu").is_none());
}
