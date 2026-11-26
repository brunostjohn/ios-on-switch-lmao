// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected IPSW's installd prepared from the OS volume's files, against the build's
//! `//drivers/SwitchInstallPolicy:installd`: the entitlements plist and the whole signed file must be the same bytes.

use sha2::{Digest, Sha256};
use switch_installd::{Error, prepare};
use switch_sign::embedded_entitlements;

fn read(variable: &str) -> Vec<u8> {
    let path = std::env::var(variable).unwrap_or_else(|_| panic!("{variable} is set by the BUILD file"));
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn build() -> String {
    std::env::var("SWITCH_INSTALLD_BUILD").expect("SWITCH_INSTALLD_BUILD is set by the BUILD file")
}

fn first_difference(a: &[u8], b: &[u8]) -> Option<usize> {
    a.iter()
        .zip(b)
        .position(|(x, y)| x != y)
        .or((a.len() != b.len()).then(|| a.len().min(b.len())))
}

#[test]
fn installd_matches_the_build() {
    let (original, system_version, built) = (read("INSTALLD"), read("SYSTEM_VERSION"), read("INSTALLD_BUILT"));
    let signed = prepare(&original, &system_version, &build()).unwrap();

    let ours = embedded_entitlements(&signed.bytes)
        .unwrap()
        .expect("ours has entitlements");
    let expected = embedded_entitlements(&built)
        .unwrap()
        .expect("the build's installd has entitlements");
    assert_eq!(String::from_utf8_lossy(ours), String::from_utf8_lossy(expected));

    let first = first_difference(&signed.bytes, &built);
    println!(
        "{}: installd original {} build {} ours {} cdhash {} entitlements {} B ({})",
        build(),
        hex::encode(Sha256::digest(&original)),
        hex::encode(Sha256::digest(&built)),
        hex::encode(Sha256::digest(&signed.bytes)),
        hex::encode(signed.cdhash),
        ours.len(),
        first.map_or("equal".to_owned(), |at| format!("DIFFER at {at:#x}")),
    );
    assert!(first.is_none(), "the prepared installd differs from the build's");
}

/// A universal file (32-bit big-endian table) holding only `image`, at 16 KiB.
fn universal(image: &[u8]) -> Vec<u8> {
    const AT: usize = 1 << 14;
    let mut out = vec![0; AT];
    let cpusubtype = u32::from_le_bytes(image[8..12].try_into().unwrap());
    for (at, word) in [
        0xCAFE_BABE,
        1,
        0x0100_000C,
        cpusubtype,
        AT as u32,
        image.len() as u32,
        14,
    ]
    .into_iter()
    .enumerate()
    {
        out[at * 4..at * 4 + 4].copy_from_slice(&u32::to_be_bytes(word));
    }
    out.extend_from_slice(image);
    out
}

#[test]
fn a_universal_installd_gives_the_build_s_file() {
    let (original, system_version, built) = (read("INSTALLD"), read("SYSTEM_VERSION"), read("INSTALLD_BUILT"));
    let thin = switch_installd::arm64_slice(&original).unwrap();
    let signed = prepare(&universal(thin), &system_version, &build()).unwrap();
    assert!(
        signed.bytes == built,
        "the universal installd's result differs from the build's"
    );
}

#[test]
fn another_builds_system_version_is_refused() {
    let mut version: plist::Dictionary = plist::from_bytes(&read("SYSTEM_VERSION")).unwrap();
    version.insert("ProductBuildVersion".into(), "99Z999".into());
    let mut other = Vec::new();
    plist::to_writer_xml(&mut other, &version).unwrap();
    let error = prepare(&read("INSTALLD"), &other, &build()).err().expect("refused");
    println!("{error}");
    assert!(matches!(error, Error::Build { found: Some(ref f), .. } if f == "99Z999"));
}
