// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_plistlib against the build's Python plistlib (oracle.py): every value read back from plistlib's binary or
//! unsorted XML must be written as the same bytes as `plistlib.dumps`.

use std::io::Cursor;

fn cases() -> Vec<(Vec<u8>, Vec<u8>)> {
    let path = std::env::var("ORACLE_CASES").expect("ORACLE_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut pieces = Vec::new();
    let mut at = 0;
    while at < data.len() {
        let length = u32::from_be_bytes(data[at..at + 4].try_into().unwrap()) as usize;
        pieces.push(data[at + 4..at + 4 + length].to_vec());
        at += 4 + length;
    }
    let mut pieces = pieces.into_iter();
    std::iter::from_fn(|| Some((pieces.next()?, pieces.next()?))).collect()
}

#[test]
fn matches_plistlib() {
    let cases = cases();
    assert!(cases.len() > 20, "the oracle wrote {} cases", cases.len());
    for (index, (source, expected)) in cases.iter().enumerate() {
        let value = plist::Value::from_reader(Cursor::new(source)).unwrap_or_else(|e| panic!("case {index}: {e}"));
        let ours = switch_plistlib::to_xml(&value).unwrap_or_else(|e| panic!("case {index}: {e}"));
        assert_eq!(
            String::from_utf8_lossy(&ours),
            String::from_utf8_lossy(expected),
            "case {index}"
        );
    }
    println!("{} cases equal to plistlib.dumps", cases.len());
}

#[test]
fn refuses_what_plistlib_refuses() {
    for text in ["nul\0", "bell\x07", "escape\x1b"] {
        let value = plist::Value::String(text.into());
        assert!(matches!(
            switch_plistlib::to_xml(&value),
            Err(switch_plistlib::Error::ControlCharacter)
        ));
    }
    let uid = plist::Value::Uid(plist::Uid::new(1));
    assert!(matches!(
        switch_plistlib::to_xml(&uid),
        Err(switch_plistlib::Error::Uid)
    ));
}
