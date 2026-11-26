// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;

use sha2::{Digest, Sha256};
use switch_apfs_write::{Attrs, Tree};

use crate::archive::{normalize_archive_name, read_and_validate_archive};
use crate::tests::{TarSpec, hard, make_package_bytes, make_tar, reg, sym};
use crate::{BootstrapInput, BootstrapPins, HELD, parse_manifest, stage_bootstrap, unpacked_bytes};

#[test]
fn archive_path_normalization_matches_python() {
    assert_eq!(normalize_archive_name(".").unwrap(), "");
    assert_eq!(normalize_archive_name("./").unwrap(), "");
    assert_eq!(normalize_archive_name("././").unwrap(), "");
    assert_eq!(normalize_archive_name("./usr/bin/tool").unwrap(), "usr/bin/tool");
    assert_eq!(normalize_archive_name("usr/bin/").unwrap(), "usr/bin");

    for bad in [
        "/outside",
        "../outside",
        "a/../b",
        "a/./b",
        "a//b",
        "a/.",
        "a\0b",
        &"a".repeat(1024),
    ] {
        assert!(normalize_archive_name(bad).is_err(), "should refuse {bad:?}");
    }
}

fn raw_tar_entry(name: &[u8], etype: u8, mode: u32, uid: u32, gid: u32, linkname: &[u8]) -> Vec<u8> {
    let mut block = [0u8; 512];
    block[..name.len()].copy_from_slice(name);
    let mode_s = format!("{mode:07o}\0");
    block[100..108].copy_from_slice(mode_s.as_bytes());
    let uid_s = format!("{uid:07o}\0");
    block[108..116].copy_from_slice(uid_s.as_bytes());
    let gid_s = format!("{gid:07o}\0");
    block[116..124].copy_from_slice(gid_s.as_bytes());
    block[124..136].copy_from_slice(b"00000000000\0");
    block[136..148].copy_from_slice(b"00000000000\0");
    block[148..156].fill(b' ');
    block[156] = etype;
    block[157..157 + linkname.len()].copy_from_slice(linkname);
    block[257..263].copy_from_slice(b"ustar\0");
    block[263..265].copy_from_slice(b"00");
    let cksum: u32 = block.iter().map(|&b| b as u32).sum();
    let ck_s = format!("{cksum:06o}\0 ");
    block[148..156].copy_from_slice(ck_s.as_bytes());
    let mut out = block.to_vec();
    out.extend_from_slice(&[0u8; 1024]);
    out
}

#[test]
fn archive_refuses_escape_symlink_ancestor_duplicates_and_bad_links() {
    let helper = b"helper";
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };

    let (m, a) = make_package_bytes(
        &[sym("escape", "/tmp"), reg("escape/outside", b"x", 0o644, 0, 0)],
        helper,
        None,
    );
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    assert!(
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &m,
                archive: &a,
                helper: Some(helper),
            },
            &pins,
        )
        .is_err()
    );

    let (m, a) = make_package_bytes(
        &[reg("dup", b"1", 0o644, 0, 0), reg("dup", b"2", 0o644, 0, 0)],
        helper,
        None,
    );
    assert!(
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &m,
                archive: &a,
                helper: Some(helper),
            },
            &pins,
        )
        .is_err()
    );

    let (m, a) = make_package_bytes(&[hard("usr/bin/bad-hard", "usr/bin/tool", 0o644, 0, 0)], helper, None);
    assert!(
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &m,
                archive: &a,
                helper: Some(helper),
            },
            &pins,
        )
        .is_err()
    );

    let (m, a) = make_package_bytes(
        &[hard("usr/bin/dangling", "usr/bin/missing", 0o755, 0, 0)],
        helper,
        None,
    );
    assert!(
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &m,
                archive: &a,
                helper: Some(helper),
            },
            &pins,
        )
        .is_err()
    );

    let (m, a) = make_package_bytes(
        &[TarSpec {
            name: "fifo",
            kind: tar::EntryType::Fifo,
            mode: 0o644,
            uid: 0,
            gid: 0,
            data: b"",
            link: None,
        }],
        helper,
        None,
    );
    assert!(
        stage_bootstrap(
            &mut tree,
            &BootstrapInput {
                manifest_json: &m,
                archive: &a,
                helper: Some(helper),
            },
            &pins,
        )
        .is_err()
    );

    assert!(read_and_validate_archive(Cursor::new(raw_tar_entry(b"/outside", b'0', 0o644, 0, 0, b"")), None).is_err());
    assert!(read_and_validate_archive(Cursor::new(raw_tar_entry(b"../escape", b'0', 0o644, 0, 0, b"")), None).is_err());
    assert!(read_and_validate_archive(Cursor::new(raw_tar_entry(b".", b'0', 0o644, 0, 0, b"")), None).is_err());
    assert!(read_and_validate_archive(Cursor::new(raw_tar_entry(b"a", b'0', 0o100644, 0, 0, b"")), None).is_err());
    assert!(read_and_validate_archive(Cursor::new(raw_tar_entry(b"ln", b'2', 0o777, 0, 0, b"")), None).is_err());
    assert!(
        read_and_validate_archive(
            Cursor::new(raw_tar_entry(b"hl", b'1', 0o644, 0, 0, b"../outside")),
            None
        )
        .is_err()
    );
}

#[test]
fn missing_required_markers_are_refused() {
    let helper = b"helper";
    let pins = BootstrapPins {
        build: "21H461",
        version: "17.7.11",
    };

    for keep_first in [true, false] {
        let mut specs = vec![reg(
            "Library/dpkg/status",
            crate::tests::SAMPLE_STATUS.as_bytes(),
            0o644,
            0,
            0,
        )];
        if keep_first {
            specs.push(reg(".procursus_strapped", b"", 0o644, 0, 0));
        } else {
            specs.push(reg(".switch-bootstrap-ready", b"", 0o644, 0, 0));
        }
        let archive = make_tar(&specs);
        let manifest = serde_json::to_vec(&serde_json::json!({
            "schema_version": 1,
            "archive_sha256": hex::encode(Sha256::digest(&archive)),
            "helper_sha256": hex::encode(Sha256::digest(helper)),
            "holds": HELD,
        }))
        .unwrap();
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
        assert!(err.to_string().contains("payload lacks regular"));
    }
}

#[test]
fn manifest_validation_refuses_bad_schema_holds_or_generation() {
    let good_hash = "a".repeat(64);
    let base = serde_json::json!({
        "schema_version": 1,
        "archive_sha256": good_hash,
        "helper_sha256": good_hash,
        "holds": HELD,
    });
    assert!(parse_manifest(&serde_json::to_vec(&base).unwrap(), None).is_ok());

    for mut bad in [
        {
            let mut v = base.clone();
            v["schema_version"] = serde_json::json!(2);
            v
        },
        {
            let mut v = base.clone();
            v["archive_sha256"] = serde_json::json!("A".repeat(64));
            v
        },
        {
            let mut v = base.clone();
            v["holds"] = serde_json::json!(["org.coolstar.sileo", "ellekit", "uikittools"]);
            v
        },
        {
            let mut v = base.clone();
            v["managed_generation"] = serde_json::json!("not-a-sha256");
            v
        },
        {
            let mut v = base.clone();
            v["managed_generation"] = serde_json::Value::Null;
            v
        },
    ] {
        assert!(
            parse_manifest(&serde_json::to_vec(&bad).unwrap(), None).is_err(),
            "should refuse {bad}"
        );
        let _ = &mut bad;
    }
}

#[test]
fn unpacked_bytes_rounds_regular_files_to_4k_blocks() {
    let archive = make_tar(&[
        reg("empty", b"", 0o644, 0, 0),
        reg("small", b"abc", 0o644, 0, 0),
        reg("exact", &[0u8; 4096], 0o644, 0, 0),
        reg("over", &[0u8; 4097], 0o644, 0, 0),
        sym("ln", "small"),
        hard("hl", "small", 0o644, 0, 0),
    ]);
    assert_eq!(unpacked_bytes(Cursor::new(archive)).unwrap(), 4096 + 4096 + 8192);
}
