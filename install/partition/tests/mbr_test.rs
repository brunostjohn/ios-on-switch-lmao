// SPDX-License-Identifier: GPL-3.0-or-later
//! hekate's MBR-only cards (the FAT, then L4T and emuMMC partitions) get a GPT and the hybrid MBR hekate writes
//! itself: slot 0 the FAT exactly, emuMMC after it, then the protective entry; L4T lives in the GPT alone.

use switch_card::{BASIC_DATA_TYPE, Card, EMUMMC_TYPE, LINUX_TYPE};
use switch_card_testing::SparseImage;
use switch_partition::Error;
use switch_partition_testing::cards::FAT_FIRST;
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, Shape};
use switch_partition_testing::{macos, reader, scenario};

const CARD: u64 = 1 << 20;
const L4T: (u8, u64, u64) = (0x83, FAT_FIRST + SMALL_FAT, 1 << 18);
const EMU: (u8, u64, u64) = (0xe0, FAT_FIRST + SMALL_FAT + (1 << 18), 1 << 17);
const EMU2: (u8, u64, u64) = (0xe0, FAT_FIRST + SMALL_FAT + (3 << 17), 1 << 17);

const SHAPE: Shape = Shape {
    root_in_cut: false,
    full_tail: false,
    fragments: 0,
    filler: true,
};

fn slot(mbr: &[u8; 512], i: usize) -> (u8, u64, u64) {
    let p = &mbr[446 + 16 * i..462 + 16 * i];
    let le = |at: usize| u64::from(u32::from_le_bytes(p[at..at + 4].try_into().unwrap()));
    (p[4], le(8), le(12))
}

/// Partitions an MBR-only card with `rest` after the FAT and checks the GPT entries it ends with and its MBR.
fn convert(name: &str, rest: &[(u8, u64, u64)], names: &[&str], hybrid: &[(u8, u64, u64)]) {
    convert_sized(name, SMALL_FAT, rest, names, hybrid);
}

fn convert_sized(name: &str, fat_sectors: u64, rest: &[(u8, u64, u64)], names: &[&str], hybrid: &[(u8, u64, u64)]) {
    let mut image = fixtures::mbr_only_sized(CARD, fat_sectors, rest, &SHAPE);
    let original_mbr = image.sector(0);
    let before = reader::list(&mut image, FAT_FIRST, 0);
    assert!(matches!(Card::open(&mut image), Err(switch_card::Error::NoGpt)));
    let (plan, report) = scenario::partition(&mut image, SMALL_IOS);
    assert!(plan.new_gpt.is_some() && plan.entry_index == rest.len() as u32 + 1);
    scenario::check_done(&mut image, &plan, &before, 0, name);
    let card = Card::open(&mut image).unwrap();
    let got: Vec<String> = card.entries().iter().map(|e| e.name()).collect();
    assert_eq!(got, names, "{name}: GPT entries");
    for (e, &(kind, first, len)) in card.entries()[1..].iter().zip(rest) {
        let type_guid = if kind == 0x83 { LINUX_TYPE } else { EMUMMC_TYPE };
        assert_eq!(
            (e.type_guid(), e.first(), e.sectors()),
            (type_guid, first, len),
            "{name}: {e:?}"
        );
    }
    assert_eq!(card.entries()[0].type_guid(), BASIC_DATA_TYPE);
    let mbr = image.sector(0);
    let slots: Vec<(u8, u64, u64)> = (0..4).map(|i| slot(&mbr, i)).filter(|s| s.0 != 0).collect();
    assert_eq!(slots, hybrid, "{name}: hybrid MBR");
    assert_eq!(
        &mbr[..446],
        &original_mbr[..446],
        "{name}: boot code and disk signature"
    );
    assert_eq!(
        &mbr[446..462][..12],
        &original_mbr[446..462][..12],
        "{name}: slot 0 apart from its length"
    );
    let fsck = macos::fsck_fat32(&image, FAT_FIRST, plan.fat_new_sectors, name);
    eprintln!("{name}: moved {} KiB; fsck_msdos -n:\n{fsck}", report.moved_bytes >> 10);
}

fn fat(plan_sectors: u64) -> (u8, u64, u64) {
    (0x0c, FAT_FIRST, plan_sectors)
}

const FAT_NEW: u64 = (FAT_FIRST + SMALL_FAT) / 0x8000 * 0x8000 - (SMALL_IOS / 512) - FAT_FIRST;
const PROTECTIVE: (u8, u64, u64) = (0xee, 1, CARD - 1);

#[test]
fn the_fat_alone() {
    convert("fat-only", &[], &["hos_data", "iOS-setup"], &[fat(FAT_NEW), PROTECTIVE]);
}

/// hekate's FAT-only card: f_mkfs runs the FAT to the card's last sector, where the backup GPT has to go.
#[test]
fn the_fat_alone_to_the_last_sector() {
    let fat_sectors = CARD - FAT_FIRST;
    let fat_new = (CARD - 33) / 0x8000 * 0x8000 - SMALL_IOS / 512 - FAT_FIRST;
    convert_sized(
        "fat-to-the-end",
        fat_sectors,
        &[],
        &["hos_data", "iOS-setup"],
        &[fat(fat_new), PROTECTIVE],
    );
}

#[test]
fn the_fat_l4t_and_emummc() {
    convert(
        "fat-l4t-emummc",
        &[L4T, EMU],
        &["hos_data", "l4t", "emummc", "iOS-setup"],
        &[fat(FAT_NEW), EMU, PROTECTIVE],
    );
}

#[test]
fn every_slot_used_l4t_gives_its_slot_up() {
    convert(
        "fat-l4t-two-emummc",
        &[L4T, EMU, EMU2],
        &["hos_data", "l4t", "emummc", "emummc2", "iOS-setup"],
        &[fat(FAT_NEW), EMU, EMU2, PROTECTIVE],
    );
}

fn refused(mut image: SparseImage) -> Error {
    let original = image.clone();
    let err = switch_partition::survey(&mut image)
        .err()
        .expect("refused at the survey");
    assert!(image == original);
    err
}

#[test]
fn cards_hekate_did_not_make_are_refused_before_any_write() {
    let three_emummc = [(0xe0, L4T.1, L4T.2), EMU, EMU2];
    let full = fixtures::mbr_only(CARD, &three_emummc, &SHAPE);
    assert!(matches!(refused(full), Error::Mbr(_)));
    let l4t_to_the_end = fixtures::mbr_only(CARD, &[(0x83, L4T.1, CARD - L4T.1)], &SHAPE);
    assert!(matches!(refused(l4t_to_the_end), Error::Mbr(_)));
    let ntfs = fixtures::mbr_only(CARD, &[(0x07, L4T.1, L4T.2)], &SHAPE);
    assert!(matches!(refused(ntfs), Error::Mbr(_)));
    let mut stale = fixtures::mbr_only(CARD, &[L4T], &SHAPE);
    stale.set(1, b"EFI PART");
    assert!(matches!(refused(stale), Error::Mbr(_)));
}
