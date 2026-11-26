// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::sync::Arc;

use switch_apfs_format::fs_records::UF_COMPRESSED;
use switch_apfs_write::Content;
use switch_apfs_write::target::MemTarget;
use tempfile::TempDir;

use crate::tests::{sha256, system_with_var};
use crate::{
    DeltaDir, DeltaEntry, Error, Replacement, apply_var_delta, is_staged_app, is_var_delta_entry, load_manifest,
    seed_data, tree_entries, var_entries,
};

#[test]
fn delta_filtering_splits_var_and_tree_entries() {
    let mk = |path: &str| DeltaEntry {
        path: path.into(),
        file: format!("files{path}"),
        sha256_in: [1; 32],
        sha256_out: [2; 32],
        cdhash: vec![],
    };
    let all = vec![
        mk("/usr/libexec/tool"),
        mk("/private/var/db/plain"),
        mk("/private/var/staged_system_apps/App.app/App"),
        mk("/System/Library/Frameworks/Foo"),
    ];
    assert!(is_var_delta_entry("/private/var/db/plain"));
    assert!(!is_var_delta_entry("/private/var/staged_system_apps/App.app/App"));
    assert!(is_staged_app("/private/var/staged_system_apps/App.app/App"));
    assert!(!is_var_delta_entry("/usr/libexec/tool"));

    let vars: Vec<_> = var_entries(&all).into_iter().map(|e| e.path).collect();
    assert_eq!(vars, ["/private/var/db/plain"]);
    let trees: Vec<_> = tree_entries(&all).into_iter().map(|e| e.path).collect();
    assert_eq!(
        trees,
        [
            "/usr/libexec/tool",
            "/private/var/staged_system_apps/App.app/App",
            "/System/Library/Frameworks/Foo",
        ]
    );
}

#[test]
fn apply_var_delta_replaces_plain_compressed_and_hardlinks_and_skips_staged_apps() {
    let (sys, orig) = system_with_var();
    let fs = sys.fs();
    let mut seeded = seed_data(&fs).unwrap();

    let plain_ino_before = seeded.tree.number(seeded.tree.lookup("db/plain").unwrap());
    let inline_ino_before = seeded.tree.number(seeded.tree.lookup("db/comp_inline").unwrap());
    let rsrc_ino_before = seeded.tree.number(seeded.tree.lookup("db/comp_rsrc").unwrap());
    let hl1_ref = seeded.tree.lookup("db/hl1").unwrap();
    let hl2_ref = seeded.tree.lookup("db/hl2").unwrap();
    assert_eq!(hl1_ref, hl2_ref);

    let new_plain = b"translated plain\n".to_vec();
    let new_inline = b"translated inline\n".to_vec();
    let new_rsrc = b"translated rsrc\n".to_vec();
    let new_hl = b"translated hardlink\n".to_vec();
    let new_app = b"translated app\n".to_vec();
    let new_usr = b"translated usr\n".to_vec();

    let repl: HashMap<String, Vec<u8>> = HashMap::from([
        ("/private/var/db/plain".into(), new_plain),
        ("/private/var/db/comp_inline".into(), new_inline.clone()),
        ("/private/var/db/comp_rsrc".into(), new_rsrc.clone()),
        ("/private/var/db/hl1".into(), new_hl.clone()),
        ("/private/var/db/hl2".into(), new_hl.clone()),
        ("/private/var/staged_system_apps/App.app/App".into(), new_app),
        ("/usr/libexec/tool".into(), new_usr),
    ]);

    let mk = |path: &str| DeltaEntry {
        path: path.into(),
        file: format!("files{path}"),
        sha256_in: sha256(&orig[path]),
        sha256_out: sha256(&repl[path]),
        cdhash: vec![],
    };
    let entries = vec![
        mk("/usr/libexec/tool"),
        mk("/private/var/db/plain"),
        mk("/private/var/db/comp_inline"),
        mk("/private/var/db/comp_rsrc"),
        mk("/private/var/db/hl1"),
        mk("/private/var/db/hl2"),
        mk("/private/var/staged_system_apps/App.app/App"),
    ];
    let loader = |e: &DeltaEntry| Ok(Replacement::Bytes(Arc::from(repl[&e.path].clone())));

    let stats = apply_var_delta(&mut seeded.tree, &fs, &entries, &loader).unwrap();
    assert_eq!(stats.matched, 5);
    assert_eq!(stats.replaced, 4);
    assert_eq!(stats.hard_links, 1);

    let t = &seeded.tree;
    assert_eq!(t.number(t.lookup("db/plain").unwrap()), plain_ino_before);

    let comp_inline = t.inode(t.lookup("db/comp_inline").unwrap());
    assert_eq!(t.number(t.lookup("db/comp_inline").unwrap()), inline_ino_before);
    assert_eq!(
        (comp_inline.attrs.uid, comp_inline.attrs.gid, comp_inline.attrs.mode),
        (0, 80, 0o755)
    );
    assert_eq!(comp_inline.attrs.bsd_flags & UF_COMPRESSED, 0);
    assert_eq!(comp_inline.attrs.bsd_flags & 0x8000, 0x8000);
    assert!(comp_inline.xattrs.is_empty());
    assert_eq!(t.stream(comp_inline.data.unwrap()).size, new_inline.len() as u64);

    let comp_rsrc = t.inode(t.lookup("db/comp_rsrc").unwrap());
    assert_eq!(t.number(t.lookup("db/comp_rsrc").unwrap()), rsrc_ino_before);
    assert_eq!(comp_rsrc.attrs.bsd_flags & UF_COMPRESSED, 0);
    assert!(comp_rsrc.xattrs.is_empty());
    assert_eq!(t.stream(comp_rsrc.data.unwrap()).size, new_rsrc.len() as u64);

    let hl = t.inode(hl1_ref);
    assert_eq!((hl.attrs.uid, hl.attrs.gid, hl.attrs.mode), (64, 64, 0o640));
    assert_eq!(t.stream(hl.data.unwrap()).size, new_hl.len() as u64);

    let app = t.inode(t.lookup("staged_system_apps/App.app/App").unwrap());
    assert!(matches!(t.stream(app.data.unwrap()).content, Content::Source(_)));
}

#[test]
fn apply_var_delta_refuses_bad_digests_and_conflicting_hardlinks() {
    let (sys, orig) = system_with_var();
    let fs = sys.fs();
    let good_out = b"translated\n".to_vec();

    let mut seeded = seed_data(&fs).unwrap();
    let bad_in = vec![DeltaEntry {
        path: "/private/var/db/plain".into(),
        file: "files/private/var/db/plain".into(),
        sha256_in: [0xAA; 32],
        sha256_out: sha256(&good_out),
        cdhash: vec![],
    }];
    let loader = |_: &DeltaEntry| Ok(Replacement::Bytes(Arc::from(good_out.clone())));
    assert!(matches!(
        apply_var_delta(&mut seeded.tree, &fs, &bad_in, &loader),
        Err(Error::InputDigestMismatch { .. })
    ));

    let mut seeded = seed_data(&fs).unwrap();
    let bad_out = vec![DeltaEntry {
        path: "/private/var/db/plain".into(),
        file: "files/private/var/db/plain".into(),
        sha256_in: sha256(&orig["/private/var/db/plain"]),
        sha256_out: [0xBB; 32],
        cdhash: vec![],
    }];
    assert!(matches!(
        apply_var_delta(&mut seeded.tree, &fs, &bad_out, &loader),
        Err(Error::OutputDigestMismatch { .. })
    ));

    let mut seeded = seed_data(&fs).unwrap();
    let conflict = vec![
        DeltaEntry {
            path: "/private/var/db/hl1".into(),
            file: "files/private/var/db/hl1".into(),
            sha256_in: sha256(&orig["/private/var/db/hl1"]),
            sha256_out: sha256(b"way one"),
            cdhash: vec![],
        },
        DeltaEntry {
            path: "/private/var/db/hl2".into(),
            file: "files/private/var/db/hl2".into(),
            sha256_in: sha256(&orig["/private/var/db/hl2"]),
            sha256_out: sha256(b"way two"),
            cdhash: vec![],
        },
    ];
    let loader_hl = |e: &DeltaEntry| {
        let bytes = if e.path.ends_with("hl1") {
            b"way one".to_vec()
        } else {
            b"way two".to_vec()
        };
        Ok(Replacement::Bytes(Arc::from(bytes)))
    };
    assert!(matches!(
        apply_var_delta(&mut seeded.tree, &fs, &conflict, &loader_hl),
        Err(Error::ConflictingDelta { .. })
    ));
}

#[test]
fn apply_var_delta_loads_from_delta_dir_on_disk() {
    let (sys, orig) = system_with_var();
    let fs = sys.fs();
    let mut seeded = seed_data(&fs).unwrap();

    let dir = TempDir::new().unwrap();
    let repl_bytes = b"on-disk replacement\n".to_vec();
    let rel = "files/private/var/db/comp_inline";
    let full = dir.path().join(rel);
    std::fs::create_dir_all(full.parent().unwrap()).unwrap();
    std::fs::write(&full, &repl_bytes).unwrap();

    let manifest_json = format!(
        "{{\"files\":[{{\"path\":\"/private/var/db/comp_inline\",\"file\":\"{rel}\",\"sha256_in\":\"{}\",\"sha256_out\":\"{}\"}}]}}",
        hex::encode(sha256(&orig["/private/var/db/comp_inline"])),
        hex::encode(sha256(&repl_bytes)),
    );
    let manifest_path = dir.path().join("os.manifest.json");
    std::fs::write(&manifest_path, manifest_json).unwrap();

    let entries = load_manifest(&manifest_path).unwrap();
    let stats = apply_var_delta(&mut seeded.tree, &fs, &entries, &DeltaDir(dir.path().to_path_buf())).unwrap();
    assert_eq!(stats.replaced, 1);

    let inode = seeded.tree.inode(seeded.tree.lookup("db/comp_inline").unwrap());
    let stream = seeded.tree.stream(inode.data.unwrap());
    assert_eq!(stream.size, repl_bytes.len() as u64);
    let Content::Fill(fill) = &stream.content else {
        panic!("expected FileFill");
    };
    let mut target = MemTarget::default();
    fill.fill(&mut target).unwrap();
    assert_eq!(&target.blocks[&0][..repl_bytes.len()], &repl_bytes[..]);
    assert!(target.blocks[&0][repl_bytes.len()..].iter().all(|&b| b == 0));
}
