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

/// Names macOS 27.2 created on a case-sensitive volume, and the `name_len_and_hash` it stored for each: Hangul, both
/// precomposed and as jamo; combining marks that reorder; emoji sequences; CJK with compatibility singletons; and
/// letters new in Unicode 16.0 and 17.0.
const MACOS_27_NAMES: &[(&[u32], u32)] = &[
    (&[0xD55C, 0xAE00, 0xAC01], 0x2741c9),
    (&[0x1112, 0x1161, 0x11AB], 0x10ad04),
    (&[0xE9, 0xF1, 0xFC, 0x1EC7], 0x3dc82b),
    (&[0x61, 0x308, 0x323, 0x301], 0x3d22e4),
    (&[0x1F600, 0x1F680], 0x0f1ce3),
    (&[0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467], 0x0a5b89),
    (&[0x1F44D, 0x1F3FD], 0x084149),
    (&[0x1F1EF, 0x1F1F5], 0x399008),
    (&[0x2764, 0xFE0F], 0x34307d),
    (&[0x6F22, 0x5B57, 0x65E5, 0x672C], 0x12df22),
    (&[0x20000, 0x2A6D6], 0x2b928f),
    (&[0xF900, 0xF91D], 0x330063),
    (&[0x2F800, 0x2FA1D], 0x10f60e),
    (&[0x2126, 0x212B, 0x212A], 0x18dec1),
    (&[0x41, 0x2000, 0x2001, 0x42], 0x076017),
    (&[0x386, 0x1F71, 0x385], 0x05787f),
    (&[0x915, 0x93C, 0x958], 0x326351),
    (&[0xF73, 0xF75, 0xF81], 0x288cd6),
    (&[0x1D15E, 0x1D160], 0x03d681),
    (&[0x41, 0x61, 0x130, 0x131, 0x3A3, 0x3C2], 0x046aed),
    (&[0xC5], 0x267b74),
    (&[0x41, 0x30A, 0x5F], 0x0940e4),
    (&[0x10D40, 0x10D41], 0x269c9d),
    (&[0x11380, 0x113B8, 0x113C2], 0x04a8e7),
    (&[0x7A, 0x897, 0x316], 0x00c708),
    (&[0x16D40, 0x16D63, 0x16D67], 0x29c2cc),
    (&[0x79, 0x1ACF, 0x316], 0x1919e3),
    (&[0x323B0, 0x323B1], 0x2bdfcc),
    (&[0x10940, 0x10941], 0x18fd56),
    (&[0x1FAC8], 0x22c4bd),
];

/// The first entry of `MACOS_27_NAMES` that holds a letter Unicode 15.1 doesn't assign.
const FIRST_AFTER_15_1: usize = 22;

#[test]
fn macos_27_non_ascii_names() {
    for &(codes, stored) in MACOS_27_NAMES {
        let name = s(codes);
        let packed = name_len_and_hash(&name, V17).unwrap_or_else(|e| panic!("{codes:x?}: {e}"));
        assert_eq!(packed, stored << 10 | (name.len() as u32 + 1), "{codes:x?}");
    }
}

#[test]
fn ios_17_refuses_letters_after_15_1() {
    for (i, &(codes, _)) in MACOS_27_NAMES.iter().enumerate() {
        let got = check(&s(codes), V15);
        if i < FIRST_AFTER_15_1 {
            assert_eq!(got, Ok(()), "{codes:x?}");
        } else {
            assert!(matches!(got, Err(NameError::Unassigned { .. })), "{codes:x?}: {got:?}");
        }
    }
}
