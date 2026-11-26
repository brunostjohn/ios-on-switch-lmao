// SPDX-License-Identifier: GPL-3.0-or-later
//! The owner's 1 TB card (install/testdata/switch-card-1tb.json) and the layouts the kernel's sd_layout_test.cpp
//! boots on it, with the same numbers.

use base64::Engine;
use switch_card::APFS_TYPE;

use crate::image::SparseImage;
use crate::tables::{get_entry, mbr_slot, rewrite, set_entry};

pub const SECTORS: u64 = 1_023_871_549_440 / 512;
pub const FAT_FIRST: u64 = 32768;
pub const OLD_FAT_SECTORS: u64 = 1_730_705_408;
pub const OLD_FAT_END: u64 = FAT_FIRST + OLD_FAT_SECTORS;
const IOS_SECTORS: u64 = 64 << 21;
const ALIGN: u64 = 0x8000;
pub const NEW_FAT_END: u64 = (OLD_FAT_END - IOS_SECTORS) / ALIGN * ALIGN;
pub const NEW_FAT_SECTORS: u64 = NEW_FAT_END - FAT_FIRST;
pub const USERDATA: u32 = 8;
pub const APPENDED: u32 = 10;
pub const USERDATA_FIRST: u64 = 1_810_368_512;
pub const USERDATA_LAST: u64 = 1_877_477_375;

/// The card's tables before its first iOS install, read from the JSON at `path`.
pub fn before(path: &str) -> SparseImage {
    let json: serde_json::Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    assert_eq!(json["disk_bytes"].as_u64(), Some(SECTORS * 512));
    let mut image = SparseImage::new(SECTORS);
    for run in json["sectors"].as_array().unwrap() {
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(run["base64"].as_str().unwrap())
            .unwrap();
        assert_eq!(bytes.len() as u64, run["count"].as_u64().unwrap() * 512);
        image.set(run["lba"].as_u64().unwrap(), &bytes);
    }
    image
}

/// iOS in Android's old userdata (entry 8), APFS-typed and named `iOS`.
pub fn old_layout(path: &str) -> SparseImage {
    let mut image = before(path);
    rewrite(&mut image, 10, |a| {
        let e = get_entry(a, USERDATA).retyped(APFS_TYPE, "iOS");
        set_entry(a, USERDATA, &e);
    });
    image
}

/// The FAT-first layout: hos_data and MBR slot 0 cut to `NEW_FAT_SECTORS`, entry 10 appended in the freed tail as
/// `name`/`type_guid`, entry 8 left as `old_root` makes it (the old layout's root, or userdata again).
pub fn fat_first(path: &str, old_root: bool, type_guid: [u8; 16], name: &str) -> SparseImage {
    let mut image = if old_root { old_layout(path) } else { before(path) };
    rewrite(&mut image, 11, |a| {
        let mut fat = get_entry(a, 0);
        fat.0[40..48].copy_from_slice(&(NEW_FAT_END - 1).to_le_bytes());
        set_entry(a, 0, &fat);
        let appended = crate::tables::entry(type_guid, 0x5a, name, NEW_FAT_END, OLD_FAT_END - NEW_FAT_END);
        set_entry(a, APPENDED, &appended);
    });
    mbr_slot(&mut image, 0, 0x0c, FAT_FIRST, NEW_FAT_SECTORS);
    image
}
