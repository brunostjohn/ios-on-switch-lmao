// SPDX-License-Identifier: GPL-3.0-or-later
use super::*;

const V17: UnicodeVersion = UnicodeVersion::IOS_27;
const V15: UnicodeVersion = UnicodeVersion::IOS_17;

fn s(codes: &[u32]) -> String {
    codes.iter().map(|&c| char::from_u32(c).unwrap()).collect()
}

/// Stored hashes from Apple's volumes: 24B119's System and a Data volume macOS 27.2 made.
#[test]
fn apples_stored_hashes() {
    for (name, stored) in [
        ("root", 0x2d9c79),
        ("private-dir", 0x2b29a3),
        ("usr", 0x063f8d),
        ("Applications", 0x3a3247),
        ("hello", 0x31ebb3),
        (".fseventsd", 0x272f85),
        ("sub", 0x230e33),
    ] {
        assert_eq!(hash(name), stored, "{name}");
    }
}

#[test]
fn len_and_hash_packs_the_terminated_length() {
    assert_eq!(name_len_and_hash("hello", V17), Ok(0xc7ae_cc06));
    assert_eq!(name_len_and_hash("sub", V17), Ok(0x8c38_cc04));
}

/// Names macOS 27.2 (Unicode 17.0) created, with the hash it stored.
#[test]
fn g3_host_vectors() {
    let mut thirty_one = vec![0x61];
    thirty_one.extend([0x301; 31]);
    let mut thirty = vec![0x62];
    thirty.extend([0x301; 30]);
    for (codes, stored) in [
        (vec![0x70, 0x6c, 0x61, 0x69, 0x6e], 0x10e4c3),
        (vec![0xe9, 0x74, 0xe9], 0x2ddd9d),
        (vec![0xd55c, 0xae00], 0x3b2f9b),
        (vec![0x61, 0x301, 0x316], 0x2720fc),
        (vec![0x20000], 0x1d84d6),
        (thirty_one, 0x2da254),
        (thirty, 0xf7c12),
        (vec![0x7a, 0x897], 0x37eaf3),
        (vec![0x79, 0x897, 0x316], 0x12bb58),
        (vec![0x7a, 0x1acf], 0x3c4848),
        (vec![0x79, 0x1acf, 0x316], 0x1919e3),
        (vec![0x7a, 0x105c9], 0x3169df),
    ] {
        let name = s(&codes);
        assert_eq!(check(&name, V17), Ok(()), "{codes:x?}");
        assert_eq!(hash(&name), stored, "{codes:x?}");
    }
}

#[test]
fn refuses_what_the_kernel_refuses() {
    assert!(matches!(
        check(&s(&[0x78, 0x378]), V17),
        Err(NameError::Unassigned { code: 0x378, .. })
    ));
    assert!(matches!(
        check(&s(&[0x78, 0xfdd0]), V17),
        Err(NameError::Noncharacter { code: 0xfdd0, .. })
    ));
    assert!(matches!(
        check(&s(&[0x78, 0x1fffe]), V17),
        Err(NameError::Noncharacter { .. })
    ));
    assert!(matches!(
        check(&s(&[0x78, 0x10ffff]), V17),
        Err(NameError::Noncharacter { .. })
    ));
}

#[test]
fn version_decides_new_marks() {
    assert_eq!(check(&s(&[0x7a, 0x897]), V17), Ok(()));
    assert!(matches!(
        check(&s(&[0x7a, 0x897]), V15),
        Err(NameError::Unassigned { code: 0x897, .. })
    ));
    assert!(matches!(
        check(&s(&[0x7a, 0x1acf]), V15),
        Err(NameError::Unassigned { .. })
    ));
    assert_eq!(check(&s(&[0x31350]), V15), Ok(()));
    assert!(matches!(check(&s(&[0x323b0]), V15), Err(NameError::Unassigned { .. })));
    assert_eq!(check(&s(&[0x323b0]), V17), Ok(()));
    assert_eq!(
        check("x", UnicodeVersion { major: 18, minor: 0 }),
        Err(NameError::UnknownVersion(UnicodeVersion { major: 18, minor: 0 }))
    );
}

#[test]
fn refuses_non_names() {
    assert_eq!(check("", V17), Err(NameError::Empty));
    assert!(matches!(check("..", V17), Err(NameError::Reserved(_))));
    assert!(matches!(check("a/b", V17), Err(NameError::Separator(_))));
    assert!(matches!(check(&"x".repeat(256), V17), Err(NameError::TooLong(_))));
    assert_eq!(check(&"x".repeat(255), V17), Ok(()));
}

#[test]
fn parses_versions() {
    assert_eq!("15.1".parse(), Ok(V15));
    assert_eq!("17".parse(), Ok(V17));
}
