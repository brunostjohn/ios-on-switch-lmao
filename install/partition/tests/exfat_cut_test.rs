// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT power-cut matrices: every write, multi-sector half-tear, epoch drop, and nested resume cut across GPT
//! multi-batch moves, MBR-only conversion, FAT-to-the-end, and the owner's 1 TB card.

use switch_card_testing::{owner, tables};
use switch_partition::DEFAULT_IOS_BYTES;
use switch_partition_testing::cards::FAT_FIRST;
use switch_partition_testing::exfat_fixtures::{self, ExfatShape};
use switch_partition_testing::fixtures::{SMALL_FAT, SMALL_IOS};
use switch_partition_testing::{macos, scenario};

#[test]
fn exfat_gpt_card_with_files_moving_in_several_batches() {
    let image = exfat_fixtures::small_exfat(&ExfatShape {
        full_tail: false,
        fragments: 300,
        filler: false,
    });
    let table = scenario::matrix("exfat-gpt-moves", &image, SMALL_IOS, true, 24);
    scenario::report("exfat-gpt-moves", &table);
}

#[test]
fn exfat_mbr_only_card_getting_its_gpt() {
    let rest = [
        (0x83, FAT_FIRST + SMALL_FAT, 1 << 18),
        (0xe0, FAT_FIRST + SMALL_FAT + (1 << 18), 1 << 17),
    ];
    let image = exfat_fixtures::mbr_only_exfat(
        1 << 20,
        &rest,
        &ExfatShape {
            full_tail: false,
            fragments: 20,
            filler: false,
        },
    );
    let table = scenario::matrix("exfat-mbr-only", &image, SMALL_IOS, true, 24);
    scenario::report("exfat-mbr-only", &table);
}

#[test]
fn exfat_mbr_only_fat_to_the_end_zeros_backup_array() {
    let sectors = 1u64 << 20;
    let fat_sectors = sectors - FAT_FIRST;
    let mut image = exfat_fixtures::mbr_only_exfat_sized(
        sectors,
        fat_sectors,
        &[],
        &ExfatShape {
            full_tail: false,
            fragments: 0,
            filler: false,
        },
    );
    for lba in (sectors - 32)..=(sectors - 2) {
        image.set(lba, &[0xde; 512]);
    }
    let table = scenario::matrix("exfat-mbr-fat-to-end", &image, SMALL_IOS, true, 24);
    scenario::report("exfat-mbr-fat-to-end", &table);
}

#[test]
fn exfat_owners_card_with_a_free_tail() {
    let mut image = owner::old_layout(&std::env::var("SWITCH_CARD_JSON").unwrap());
    tables::mbr_slot(&mut image, 0, 0x07, FAT_FIRST, owner::OLD_FAT_SECTORS);
    macos::format_exfat(&mut image, FAT_FIRST, owner::OLD_FAT_SECTORS, 64);
    let table = scenario::matrix("exfat-owner-tables", &image, DEFAULT_IOS_BYTES, true, 100);
    scenario::report("exfat-owner-tables", &table);
}
