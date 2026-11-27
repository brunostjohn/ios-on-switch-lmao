// SPDX-License-Identifier: GPL-3.0-or-later
//! A power cut at every write, and with every subset of one barrier's writes missing one: each cut leaves the kernel
//! its roots as before and every file intact through either FAT, and recovery ends byte-equal to the uncut run.

use switch_card_testing::owner;
use switch_partition::DEFAULT_IOS_BYTES;
use switch_partition_testing::cards::FAT_FIRST;
use switch_partition_testing::fixtures::{self, SMALL_IOS, Shape};
use switch_partition_testing::{macos, scenario};

#[test]
fn gpt_card_with_files_moving_in_several_batches() {
    let image = fixtures::small(&Shape {
        root_in_cut: true,
        full_tail: false,
        fragments: 500,
        filler: false,
    });
    let table = scenario::matrix("gpt-moves", &image, SMALL_IOS, true, 24);
    scenario::report("gpt-moves", &table);
}

#[test]
fn mbr_only_card_getting_its_gpt() {
    let rest = [
        (0x83, FAT_FIRST + fixtures::SMALL_FAT, 1 << 18),
        (0xe0, FAT_FIRST + fixtures::SMALL_FAT + (1 << 18), 1 << 17),
    ];
    let image = fixtures::mbr_only(
        1 << 20,
        &rest,
        &Shape {
            root_in_cut: false,
            full_tail: false,
            fragments: 20,
            filler: false,
        },
    );
    let table = scenario::matrix("mbr-only", &image, SMALL_IOS, true, 24);
    scenario::report("mbr-only", &table);
}

#[test]
fn mbr_only_fat_to_the_end_zeros_backup_array() {
    let sectors = 1u64 << 20;
    let fat_sectors = sectors - FAT_FIRST;
    let mut image = fixtures::mbr_only_sized(
        sectors,
        fat_sectors,
        &[],
        &Shape {
            root_in_cut: false,
            full_tail: false,
            fragments: 0,
            filler: false,
        },
    );
    for lba in (sectors - 32)..=(sectors - 2) {
        image.set(lba, &[0xde; 512]);
    }
    let table = scenario::matrix("mbr-fat-to-end", &image, SMALL_IOS, true, 24);
    scenario::report("mbr-fat-to-end", &table);
}

#[test]
fn owners_card_with_a_free_tail() {
    let mut image = owner::old_layout(&std::env::var("SWITCH_CARD_JSON").unwrap());
    macos::format_fat32(&mut image, FAT_FIRST, owner::OLD_FAT_SECTORS, 64);
    let table = scenario::matrix("owner-tables", &image, DEFAULT_IOS_BYTES, true, 100);
    scenario::report("owner-tables", &table);
}
