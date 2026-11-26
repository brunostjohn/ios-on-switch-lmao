// SPDX-License-Identifier: GPL-3.0-or-later
use sha2::{Digest, Sha256};

use crate::{Kext, class_name, fips, stub_cap, translate_kexts};

const CASAL_X2_X3_X0: u32 = 0xC8E2_FC03;
const RETAB: u32 = 0xD65F_0FFF;
const RET: u32 = 0xD65F_03C0;
const LSE_CAS: usize = 9;

fn words(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn word(bytes: &[u8], index: usize) -> u32 {
    u32::from_le_bytes(bytes[4 * index..4 * index + 4].try_into().unwrap())
}

#[test]
fn translates_a_kext_into_its_stub_region() {
    let va = 0xFFFF_FFF0_0700_0000;
    let stubs_va = va + 0x4000;
    let mut text = words(&[CASAL_X2_X3_X0, RETAB]);
    text.resize(0x4000, 0);
    let sections = [[va, 8]];
    let starts = [va];
    let out = translate_kexts(
        &text,
        va,
        stubs_va,
        &[Kext {
            sections: &sections,
            starts: &starts,
        }],
    )
    .unwrap();
    let kext = &out.kexts[0];
    assert_eq!((kext.words, kext.changed, kext.stub_first), (2, 2, 0));
    assert!(kext.stub_end > 0 && kext.stub_end < stub_cap());
    assert_eq!(out.stubs.len(), 4 * kext.stub_end as usize);
    assert_eq!(
        word(&out.text, 0) >> 26,
        0b000101,
        "the CASAL becomes a B into the stub region"
    );
    assert_eq!(word(&out.text, 0) & 0x03FF_FFFF, ((stubs_va - va) / 4) as u32);
    assert_eq!(word(&out.text, 1), RET);
    assert_eq!(out.text[8..], text[8..]);
    assert_eq!(kext.stats.stubs[LSE_CAS], 1);
    assert_eq!(class_name(LSE_CAS), "lse.cas");
    assert!(kext.islands.is_empty());
}

#[test]
fn class_names_end_with_the_table() {
    assert_eq!(class_name(crate::CLASSES), "?");
}

fn hmac_zero_key(text: &[u8]) -> [u8; fips::MAC_LEN] {
    let inner = Sha256::new().chain_update([0x36; 64]).chain_update(text).finalize();
    Sha256::new()
        .chain_update([0x5C; 64])
        .chain_update(inner)
        .finalize()
        .into()
}

#[test]
fn fips_mac_follows_the_text_only_when_it_held() {
    let original = words(&[RETAB, RETAB]);
    let translated = words(&[RET, RET]);
    let mut mac = hmac_zero_key(&original);
    assert!(fips::rewrite(&original, &translated, &mut mac));
    assert_eq!(mac, hmac_zero_key(&translated));
    let mut stale = [7; fips::MAC_LEN];
    assert!(!fips::rewrite(&original, &translated, &mut stale));
    assert_eq!(stale, [7; fips::MAC_LEN]);
}

#[test]
fn fips_find_refuses_what_is_no_mach_o() {
    assert_eq!(fips::find(&[]), None);
    assert_eq!(fips::find(&[0; 32]), None);
}
