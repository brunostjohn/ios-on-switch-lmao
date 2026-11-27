// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;
use std::fs;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use serde::Deserialize;
use switch_apfs_write::{Attrs, Content, Tree};
use switch_image_bootstrap::{
    BootstrapInput, BootstrapPins, DIRECTORY, MANIFEST, METADATA, MIGRATION_PENDING, stage_bootstrap,
};
use switch_image_overlay::{TreeSource, tree_digest};

#[derive(Deserialize)]
struct OracleFile {
    bootstrap: Vec<BootstrapCase>,
    overlay_digest: String,
}

#[derive(Deserialize)]
struct BootstrapCase {
    name: String,
    build: String,
    version: String,
    manifest_in_b64: String,
    archive_b64: String,
    helper_b64: String,
    expected_manifest_b64: String,
    expected_metadata_b64: String,
    expected_status_b64: String,
    expected_pending_b64: Option<String>,
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
fn bootstrap_and_overlay_match_python_oracle_byte_for_byte() {
    let path = env::var("ORACLE_CASES").expect("ORACLE_CASES env var");
    let oracle: OracleFile = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();

    for case in oracle.bootstrap {
        let manifest_in = STANDARD.decode(&case.manifest_in_b64).unwrap();
        let archive = STANDARD.decode(&case.archive_b64).unwrap();
        let helper = STANDARD.decode(&case.helper_b64).unwrap();
        let expected_manifest = STANDARD.decode(&case.expected_manifest_b64).unwrap();
        let expected_metadata = STANDARD.decode(&case.expected_metadata_b64).unwrap();
        let expected_status = STANDARD.decode(&case.expected_status_b64).unwrap();
        let expected_pending = case
            .expected_pending_b64
            .as_deref()
            .map(|s| STANDARD.decode(s).unwrap());

        let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
        let pins = BootstrapPins {
            build: &case.build,
            version: &case.version,
        };
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &manifest_in,
                archive: &archive,
                helper: Some(&helper),
            },
            &pins,
        )
        .unwrap_or_else(|e| panic!("{}: {e}", case.name));

        let base = format!("{DIRECTORY}/{}", case.build);
        assert_eq!(
            read_bytes(&tree, &format!("{base}/{MANIFEST}")),
            expected_manifest,
            "{}: manifest.json mismatch",
            case.name
        );
        assert_eq!(
            read_bytes(&tree, &format!("{base}/{METADATA}")),
            expected_metadata,
            "{}: metadata.json mismatch",
            case.name
        );
        assert_eq!(
            read_bytes(&tree, &format!("{base}/root/Library/dpkg/status")),
            expected_status,
            "{}: dpkg/status mismatch",
            case.name
        );
        let pending_path = format!("{base}/{MIGRATION_PENDING}");
        match expected_pending {
            Some(want) => assert_eq!(
                read_bytes(&tree, &pending_path),
                want,
                "{}: migration.pending mismatch",
                case.name
            ),
            None => assert!(
                tree.lookup(&pending_path).is_none(),
                "{}: unexpected migration.pending",
                case.name
            ),
        }
    }

    let mut overlay = TreeSource::new();
    overlay.add_dir("etc", 0o755).unwrap();
    overlay.add_dir("usr", 0o755).unwrap();
    overlay.add_dir("usr/bin", 0o755).unwrap();
    overlay.add_file("usr/bin/tool", &b"bin"[..], 0o555).unwrap();
    overlay.add_file("etc/conf", &b"cfg"[..], 0o444).unwrap();
    overlay.add_file("etc/._conf", &b"ignored"[..], 0o644).unwrap();
    assert_eq!(tree_digest(&overlay), oracle.overlay_digest);
}
