// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

use tempfile::TempDir;

use crate::{
    AIR64_CPU_TYPE, Crawler, FAT_MAGIC, HEADER_LEN, MTLB_MAGIC, QC_PATH, air_slice, extract_embedded_header,
    format_names_json, index_names, library_path, parse_functions, parse_names_for_build, resolve_corpus,
};

type FakeFunc<'a> = (&'a str, u8, Option<(u16, u16, u16, u16)>);

fn build_fake_mtlb(funcs: &[FakeFunc<'_>]) -> Vec<u8> {
    let mut listing = Vec::new();
    listing.extend_from_slice(&(funcs.len() as u32).to_le_bytes());
    for &(name, kind, vers) in funcs {
        let mut entry = Vec::new();
        entry.extend_from_slice(b"NAME");
        entry.extend_from_slice(&((name.len() + 1) as u16).to_le_bytes());
        entry.extend_from_slice(name.as_bytes());
        entry.push(0);
        entry.extend_from_slice(b"TYPE");
        entry.extend_from_slice(&1u16.to_le_bytes());
        entry.push(kind);
        if let Some((a0, a1, m0, m1)) = vers {
            entry.extend_from_slice(b"VERS");
            entry.extend_from_slice(&8u16.to_le_bytes());
            entry.extend_from_slice(&a0.to_le_bytes());
            entry.extend_from_slice(&a1.to_le_bytes());
            entry.extend_from_slice(&m0.to_le_bytes());
            entry.extend_from_slice(&m1.to_le_bytes());
        }
        entry.extend_from_slice(b"ENDT");
        let size = (4 + entry.len()) as u32;
        listing.extend_from_slice(&size.to_le_bytes());
        listing.extend_from_slice(&entry);
    }
    let total = HEADER_LEN + listing.len();
    let mut hdr = vec![0u8; HEADER_LEN];
    hdr[..4].copy_from_slice(MTLB_MAGIC);
    hdr[0x10..0x18].copy_from_slice(&(total as u64).to_le_bytes());
    hdr[0x18..0x20].copy_from_slice(&(HEADER_LEN as u64).to_le_bytes());
    hdr[0x20..0x28].copy_from_slice(&(listing.len() as u64).to_le_bytes());
    hdr.extend_from_slice(&listing);
    hdr
}

fn build_fat_mtlb(entries: &[(u32, u32, Vec<u8>)]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(FAT_MAGIC);
    out.extend_from_slice(&(entries.len() as u32).to_be_bytes());
    let mut offset = 8 + 20 * entries.len();
    for (cpu, sub, data) in entries {
        out.extend_from_slice(&cpu.to_be_bytes());
        out.extend_from_slice(&sub.to_be_bytes());
        out.extend_from_slice(&(offset as u32).to_be_bytes());
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        out.extend_from_slice(&0u32.to_be_bytes());
        offset += data.len();
    }
    for (_, _, data) in entries {
        out.extend_from_slice(data);
    }
    out
}

#[test]
fn universal_slice_rules_match_21h461_and_24b119() {
    let air29 = build_fake_mtlb(&[("fn_air29", 2, Some((2, 9, 3, 2)))]);
    let air24 = build_fake_mtlb(&[("fn_air24", 2, Some((2, 4, 2, 4)))]);
    let fat = build_fat_mtlb(&[
        (AIR64_CPU_TYPE, 0xD, air29.clone()),
        (AIR64_CPU_TYPE, 0x8, air24.clone()),
    ]);
    assert_eq!(air_slice(&fat, Some("21H461")).unwrap(), air29.as_slice());
    assert_eq!(air_slice(&fat, Some("24B119")).unwrap(), air24.as_slice());
    assert_eq!(
        parse_names_for_build(&fat, Some("21H461")).unwrap().unwrap(),
        vec!["fn_air29"]
    );
    assert_eq!(
        parse_names_for_build(&fat, Some("24B119")).unwrap().unwrap(),
        vec!["fn_air24"]
    );
}

#[test]
fn embedded_two_plane_stand_in_parses() {
    let lib = extract_embedded_header(Path::new("drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.h")).unwrap();
    assert_eq!(lib.sha256.len(), 64);
    assert!(lib.functions >= 3);
    let funcs = parse_functions(&lib.bytes).unwrap();
    assert!(funcs.iter().any(|f| f.name == "two_plane_cover"));
}

#[test]
fn directory_crawl_extracts_root_and_embedded() {
    let src = TempDir::new().unwrap();
    let out = TempDir::new().unwrap();
    let qc_dir = src.path().join("System/Library/Frameworks/QuartzCore.framework");
    fs::create_dir_all(&qc_dir).unwrap();
    let qc_mtlb = build_fake_mtlb(&[("qc_vert", 0, Some((2, 4, 2, 4))), ("qc_frag", 1, Some((2, 4, 2, 4)))]);
    fs::write(qc_dir.join("default.metallib"), &qc_mtlb).unwrap();

    let embedded_mtlb = build_fake_mtlb(&[("emb_kern", 2, Some((2, 4, 2, 4)))]);
    let mut binary = vec![0xCF, 0xFA, 0xED, 0xFE, 0x0C, 0x00, 0x00, 0x01];
    binary.extend_from_slice(&[0u8; 24]);
    binary.extend_from_slice(&embedded_mtlb);
    binary.extend_from_slice(b"junk #include <metal_stdlib> more #include <metal_stdlib>");
    fs::write(src.path().join("usr_bin_sample"), &binary).unwrap();
    fs::write(src.path().join("ignored.png"), &qc_mtlb).unwrap();

    let mut crawler = Crawler::new(out.path()).unwrap();
    crawler.crawl_path(src.path()).unwrap();
    let inv = crawler.inventory();
    assert_eq!(inv.files, 1);
    assert_eq!(inv.embedded, 1);
    assert_eq!(inv.embedded_seeded, 1);
    assert_eq!(inv.msl_sources.get("usr_bin_sample"), Some(&2));
    assert!(
        out.path()
            .join("root/System/Library/Frameworks/QuartzCore.framework/default.metallib")
            .is_file()
    );

    let names = index_names("21H461", &out.path().join("root"), None).unwrap();
    assert_eq!(names.get("qc_vert").unwrap(), &vec![QC_PATH.to_owned()]);
    let json = format_names_json(&names).unwrap();
    assert!(json.starts_with("{\n\"qc_frag\": [\n"));
}

#[test]
fn resolve_corpus_relocates_and_guesses() {
    let mut table = BTreeMap::new();
    table.insert("qc_v".to_owned(), vec![QC_PATH.to_owned()]);
    table.insert(
        "rb_v".to_owned(),
        vec!["/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib".to_owned()],
    );
    table.insert(
        "rb_f".to_owned(),
        vec!["/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib".to_owned()],
    );
    table.insert(
        "app_fn".to_owned(),
        vec!["/private/var/staged_system_apps/Test.app/default.metallib".to_owned()],
    );

    let avail: BTreeSet<String> = table.values().flat_map(|v| v.iter().cloned()).collect();
    assert_eq!(
        library_path(
            "/private/var/containers/Bundle/Application/01234567-89ab-cdef-0123-456789abcdef/Test.app/default.metallib",
            &avail
        ),
        "/private/var/staged_system_apps/Test.app/default.metallib"
    );
    assert_eq!(
        library_path(
            "/System/Cryptexes/OS/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib",
            &avail
        ),
        "/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib"
    );

    let corpus = concat!(
        "R ok=1 samples=1 | v rb_v | f rb_f | \n",
        "S ok=1 name=rb_f k=\n",
        "R ok=1 samples=1 | v qc_v | f - | \n",
    );
    let res = resolve_corpus(&table, corpus);
    let lines: Vec<&str> = res.text.lines().collect();
    assert_eq!(
        lines[0],
        "R ok=1 samples=1 | v rb_v lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib | f rb_f lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib | "
    );
    assert_eq!(
        lines[1],
        "S ok=1 lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib name=rb_f k="
    );
    assert_eq!(lines[2], "R ok=1 samples=1 | v qc_v | f - | ");
}
