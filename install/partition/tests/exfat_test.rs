// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT partitioning across GPT cards, 512 B and 4 KiB clusters, full tails, hekate MBR-only layouts, and the
//! owner's 1 TB card (32 KiB clusters, with switch_card install).

use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, Card, EMUMMC_TYPE, LINUX_TYPE};
use switch_card_testing::{SparseImage, owner, tables};
use switch_partition::DEFAULT_IOS_BYTES;
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::exfat::Exfat;
use switch_partition_testing::exfat_fixtures::{self, ExfatShape};
use switch_partition_testing::fixtures::{SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use switch_partition_testing::oracle::{self, Files};
use switch_partition_testing::{exfat_reader, macos, scenario};

const SIMPLE: ExfatShape = ExfatShape {
    full_tail: false,
    fragments: 0,
    filler: true,
};

fn json() -> String {
    std::env::var("SWITCH_CARD_JSON").unwrap()
}

fn shrink(name: &str, mut image: SparseImage, fat_sectors: u64) -> switch_partition::Report {
    let before = exfat_reader::list(&mut image, FAT_FIRST, true);
    let roots = scenario::roots(&mut image);
    macos::fsck_exfat(&image, FAT_FIRST, fat_sectors, &format!("{name}-before"));
    let (plan, report) = scenario::partition(&mut image, SMALL_IOS);
    scenario::check_done(&mut image, &plan, &before, roots, name);
    let fsck = macos::fsck_exfat(&image, FAT_FIRST, plan.fat_new_sectors, name);
    eprintln!(
        "{name}: {} items, moved {} KiB in {} batches; fsck_exfat -n:\n{fsck}",
        before.len(),
        report.moved_bytes >> 10,
        report.batches
    );
    report
}

#[test]
fn free_tail_and_shorter_volume_keep_length() {
    let mut image = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut image, FAT_FIRST, SMALL_FAT, 1);
    let before_bm_clusters = {
        let v = Exfat::read(&image, FAT_FIRST);
        v.chain(&image, v.bitmap_first).len()
    };
    let report = shrink("exfat-free-tail", image.clone(), SMALL_FAT);
    assert_eq!((report.moved_bytes, report.batches), (0, 0));
    let (plan, _) = scenario::partition(&mut image, SMALL_IOS);
    let after_bm_clusters = {
        let v = Exfat::read(&image, FAT_FIRST);
        v.chain(&image, v.bitmap_first).len()
    };
    assert!(
        after_bm_clusters < before_bm_clusters,
        "surplus bitmap clusters should be freed on shrink ({before_bm_clusters} -> {after_bm_clusters})"
    );
    macos::fsck_exfat(&image, FAT_FIRST, plan.fat_new_sectors, "exfat-free-tail-bm");

    let mut short = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut short, FAT_FIRST, 300_000, 1);
    let before = exfat_reader::list(&mut short, FAT_FIRST, true);
    let (plan, report) = scenario::partition(&mut short, SMALL_IOS);
    assert_eq!((report.moved_bytes, report.batches), (0, 0));
    assert!(plan.fat_new_sectors > 300_000);
    scenario::check_done(&mut short, &plan, &before, 0, "exfat-short-vol");
    let boot = short.sector(FAT_FIRST);
    assert_eq!(u64::from_le_bytes(boot[72..80].try_into().unwrap()), 300_000);
    macos::fsck_exfat(&short, FAT_FIRST, 300_000, "exfat-short-vol");
}

#[test]
fn used_tail_512b_and_4k_clusters_and_mbr_0x0c() {
    let report = shrink("exfat-used-512b", exfat_fixtures::small_exfat(&SIMPLE), SMALL_FAT);
    assert!(report.moved_bytes > 2 << 20 && report.batches >= 2);

    let mut img4k = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut img4k, FAT_FIRST, SMALL_FAT, 8);
    let v = Exfat::read(&img4k, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = v.root_dir(&img4k, vec![cut + 100]);
    v.file(
        &mut img4k,
        &mut root,
        "nfc-4k.bin",
        10 * v.cluster_bytes() - 77,
        &(cut + 200..cut + 210).collect::<Vec<_>>(),
        1,
        true,
    );
    v.file(
        &mut img4k,
        &mut root,
        "chained-4k.bin",
        4 * v.cluster_bytes() - 13,
        &[cut + 300, 50, cut + 305, 55],
        2,
        false,
    );
    let mut sub = v.mkdir(&mut img4k, &mut root, "sub-nfc", vec![cut + 400, cut + 401], true);
    v.file(
        &mut img4k,
        &mut sub,
        "nested.bin",
        2 * v.cluster_bytes(),
        &[cut + 410, cut + 411],
        3,
        true,
    );
    let report4k = shrink("exfat-used-4k", img4k, SMALL_FAT);
    assert!(report4k.moved_bytes > 0);
}

#[test]
fn full_tail_moves_into_holes_below() {
    let image = exfat_fixtures::small_exfat(&ExfatShape {
        full_tail: true,
        fragments: 0,
        filler: true,
    });
    let report = shrink("exfat-full-tail", image, SMALL_FAT);
    assert!(report.moved_bytes > 60 << 20 && report.batches > 1);
}

fn slot(mbr: &[u8; 512], i: usize) -> (u8, u64, u64) {
    let p = &mbr[446 + 16 * i..462 + 16 * i];
    let le = |at: usize| u64::from(u32::from_le_bytes(p[at..at + 4].try_into().unwrap()));
    (p[4], le(8), le(12))
}

#[test]
fn mbr_only_cards_convert_to_gpt_and_hybrid_mbr() {
    let card_sectors = 1u64 << 20;
    let l4t = (0x83, FAT_FIRST + SMALL_FAT, 1 << 18);
    let emu = (0xe0, FAT_FIRST + SMALL_FAT + (1 << 18), 1 << 17);
    let emu2 = (0xe0, FAT_FIRST + SMALL_FAT + (3 << 17), 1 << 17);
    let fat_new = (FAT_FIRST + SMALL_FAT) / 0x8000 * 0x8000 - (SMALL_IOS / 512) - FAT_FIRST;
    let protective = (0xee, 1, card_sectors - 1);

    type MbrSlot = (u8, u64, u64);
    type MbrCase<'a> = (&'a str, u64, &'a [MbrSlot], &'a [&'a str], &'a [MbrSlot]);
    let cases: [MbrCase<'_>; 3] = [
        (
            "exfat-mbr-only",
            SMALL_FAT,
            &[],
            &["hos_data", "iOS-setup"],
            &[(0x07, FAT_FIRST, fat_new), protective],
        ),
        (
            "exfat-mbr-fat-to-end",
            card_sectors - FAT_FIRST,
            &[],
            &["hos_data", "iOS-setup"],
            &[
                (
                    0x07,
                    FAT_FIRST,
                    (card_sectors - 33) / 0x8000 * 0x8000 - SMALL_IOS / 512 - FAT_FIRST,
                ),
                protective,
            ],
        ),
        (
            "exfat-mbr-l4t-two-emummc",
            SMALL_FAT,
            &[l4t, emu, emu2],
            &["hos_data", "l4t", "emummc", "emummc2", "iOS-setup"],
            &[(0x07, FAT_FIRST, fat_new), emu, emu2, protective],
        ),
    ];

    for (name, fat_sectors, rest, names, hybrid) in cases {
        let mut image = exfat_fixtures::mbr_only_exfat_sized(card_sectors, fat_sectors, rest, &SIMPLE);
        if name == "exfat-mbr-fat-to-end" {
            for lba in (card_sectors - 32)..=(card_sectors - 2) {
                image.set(lba, &[0xde; 512]);
            }
        }
        let before = exfat_reader::list(&mut image, FAT_FIRST, true);
        let (plan, _) = scenario::partition(&mut image, SMALL_IOS);
        assert!(plan.new_gpt.is_some());
        scenario::check_done(&mut image, &plan, &before, 0, name);
        let card = Card::open(&mut image).unwrap();
        let got: Vec<String> = card.entries().iter().map(|e| e.name()).collect();
        assert_eq!(got, names, "{name}");
        assert_eq!(card.entries()[0].type_guid(), BASIC_DATA_TYPE);
        for (e, &(kind, first, len)) in card.entries()[1..].iter().zip(rest) {
            let tg = if kind == 0x83 { LINUX_TYPE } else { EMUMMC_TYPE };
            assert_eq!((e.type_guid(), e.first(), e.sectors()), (tg, first, len));
        }
        let mbr = image.sector(0);
        let slots: Vec<(u8, u64, u64)> = (0..4).map(|i| slot(&mbr, i)).filter(|s| s.0 != 0).collect();
        assert_eq!(slots, hybrid, "{name}");
        if name == "exfat-mbr-fat-to-end" {
            for lba in (card_sectors - 32)..=(card_sectors - 2) {
                assert_eq!(image.sector(lba), [0u8; 512], "backup GPT array LBA {lba} not zeroed");
            }
        }
        macos::fsck_exfat(&image, FAT_FIRST, plan.fat_new_sectors, name);
    }
}

#[test]
fn owners_1tb_card_with_exfat_and_install() {
    let mut image = owner::old_layout(&json());
    tables::mbr_slot(&mut image, 0, 0x07, FAT_FIRST, owner::OLD_FAT_SECTORS);
    exfat_fixtures::fill_exfat(
        &mut image,
        owner::OLD_FAT_SECTORS,
        64,
        owner::SECTORS - 2048,
        DEFAULT_IOS_BYTES,
        &ExfatShape {
            full_tail: false,
            fragments: 40,
            filler: false,
        },
    );
    let before = exfat_reader::list(&mut image, FAT_FIRST, true);
    let roots = scenario::roots(&mut image);
    assert_eq!(roots, 1);
    let (plan, report) = scenario::partition(&mut image, DEFAULT_IOS_BYTES);
    assert_eq!(plan.fat_new_sectors, owner::NEW_FAT_SECTORS);
    assert_eq!(plan.entry_first, owner::NEW_FAT_END);
    assert_eq!(plan.entry_index, owner::APPENDED);
    assert!(report.moved_bytes > 0);
    scenario::check_done(&mut image, &plan, &before, roots, "owner-exfat");
    macos::fsck_exfat(&image, FAT_FIRST, plan.fat_new_sectors, "owner-exfat");

    let mut card = Card::open(&mut image).unwrap();
    let ip = card.plan(None, 1 << 30).unwrap();
    assert_eq!(
        (ip.target.index, ip.others.clone()),
        (owner::APPENDED, vec![owner::USERDATA])
    );
    card.mark(&mut image, &ip).unwrap();
    card.finish(&mut image, &ip).unwrap();
    let root = oracle::root(&mut image).unwrap();
    assert!(root.unique && root.first == owner::NEW_FAT_END);
    assert_eq!(
        oracle::files(&mut image, root.first, root.count),
        Files::Volume {
            first: FAT_FIRST,
            count: owner::NEW_FAT_SECTORS,
            exfat: true,
        }
    );
    let card = Card::open(&mut image).unwrap();
    assert_eq!(card.roots(), vec![owner::APPENDED]);
    assert_eq!(card.entries()[owner::APPENDED as usize].type_guid(), APFS_TYPE);
    assert!(card.entries()[owner::USERDATA as usize].named("userdata"));
}
