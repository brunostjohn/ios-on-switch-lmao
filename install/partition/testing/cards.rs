// SPDX-License-Identifier: GPL-3.0-or-later
//! Card layouts: a GPT card with its boot volume at index 0, and hekate's MBR-only cards.

use switch_card::BASIC_DATA_TYPE;
use switch_card_testing::{SparseImage, tables};

pub const FAT_FIRST: u64 = 32768;

/// A GPT card of `sectors` whose only entry is the FAT32 volume (0x0C), `fat_sectors` long from LBA 32768.
pub fn gpt(sectors: u64, fat_sectors: u64) -> SparseImage {
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, fat_sectors);
    tables::card(sectors, 128, 1, &[(0, fat)])
}

/// A GPT card of `sectors` whose only entry is the exFAT volume (0x07), `fat_sectors` long from LBA 32768.
pub fn gpt_exfat(sectors: u64, fat_sectors: u64) -> SparseImage {
    let mut image = gpt(sectors, fat_sectors);
    tables::mbr_slot(&mut image, 0, 0x07, FAT_FIRST, fat_sectors);
    image
}

/// An MBR-only card of `sectors`: slot 0 a FAT32 (0x0C) from LBA 32768, then each of `rest` (type, first, sectors).
pub fn mbr_only(sectors: u64, fat_sectors: u64, rest: &[(u8, u64, u64)]) -> SparseImage {
    mbr_only_kind(sectors, fat_sectors, 0x0c, rest)
}

/// An MBR-only card of `sectors`: slot 0 an exFAT (0x07) from LBA 32768, then each of `rest`.
pub fn mbr_only_exfat(sectors: u64, fat_sectors: u64, rest: &[(u8, u64, u64)]) -> SparseImage {
    mbr_only_kind(sectors, fat_sectors, 0x07, rest)
}

fn mbr_only_kind(sectors: u64, fat_sectors: u64, slot0_kind: u8, rest: &[(u8, u64, u64)]) -> SparseImage {
    let mut image = SparseImage::new(sectors);
    let mut mbr = [0u8; 512];
    mbr[440..444].copy_from_slice(&0x5a1e_c0deu32.to_le_bytes());
    image.set(0, &mbr);
    tables::mbr_slot(&mut image, 0, slot0_kind, FAT_FIRST, fat_sectors);
    for (i, &(kind, first, len)) in rest.iter().enumerate() {
        tables::mbr_slot(&mut image, i + 1, kind, first, len);
    }
    image
}
