// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected IPSW's installd, re-signed as `tools/install_policy/installd.py` re-signs it, against the build's
//! `//drivers/SwitchInstallPolicy:installd`: the entitlements plist and the whole signed file must be the same bytes.

use std::io::Cursor;

use sha2::{Digest, Sha256};
use switch_sign::{Embedded, Metadata, embedded_entitlements, sign_macho};

fn read(variable: &str) -> Vec<u8> {
    let path = std::env::var(variable).unwrap_or_else(|_| panic!("{variable} is set by the BUILD file"));
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn first_difference(a: &[u8], b: &[u8]) -> Option<usize> {
    a.iter()
        .zip(b)
        .position(|(x, y)| x != y)
        .or((a.len() != b.len()).then(|| a.len().min(b.len())))
}

#[test]
fn installd_matches_the_build() {
    let (original, patched, built) = (read("INSTALLD"), read("INSTALLD_PATCHED"), read("INSTALLD_BUILT"));

    let source = embedded_entitlements(&original)
        .unwrap()
        .expect("installd is signed with entitlements");
    let value = plist::Value::from_reader(Cursor::new(source)).expect("installd's entitlements parse");
    let xml = switch_plistlib::to_xml(&value).unwrap();
    let expected = embedded_entitlements(&built)
        .unwrap()
        .expect("the build's installd has entitlements");
    println!(
        "entitlements: build {} ours {} ({} B)",
        hex::encode(Sha256::digest(expected)),
        hex::encode(Sha256::digest(&xml)),
        xml.len()
    );
    assert_eq!(String::from_utf8_lossy(&xml), String::from_utf8_lossy(expected));

    let options = Embedded {
        metadata: Metadata::Preserve,
        entitlements: Some(&xml),
    };
    let signed = sign_macho(&patched, &options).unwrap();
    let first = first_difference(&signed.bytes, &built);
    println!(
        "installd: patched input {} build {} ours {} cdhash {} ({})",
        hex::encode(Sha256::digest(&patched)),
        hex::encode(Sha256::digest(&built)),
        hex::encode(Sha256::digest(&signed.bytes)),
        hex::encode(signed.cdhash),
        first.map_or("equal".to_owned(), |at| format!("DIFFER at {at:#x}")),
    );
    assert!(first.is_none(), "the re-signed installd differs from the build's");
}
