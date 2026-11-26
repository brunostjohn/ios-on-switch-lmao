// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_card against the kernel's own rules (SwitchSdRoot.h, SwitchSdVolume.cpp) on the owner's 1 TB card and on
//! synthetic cards, at every cut an install can suffer. Each matrix's table goes to stderr and to the test's outputs.

mod ffi;
mod matrix;

use std::fs::OpenOptions;

use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, Card, LINUX_TYPE, StreamDevice};
use switch_card_testing::owner::{self, FAT_FIRST, NEW_FAT_END, NEW_FAT_SECTORS, OLD_FAT_END, OLD_FAT_SECTORS};
use switch_card_testing::{SparseImage, tables};

use crate::ffi::Files;
use crate::matrix::{NEED, Scenario};

fn json() -> String {
    std::env::var("SWITCH_CARD_JSON").unwrap()
}

/// The kernel tests' FAT32 boot sector, cut as sd_layout_test.cpp cuts it: FATSz32 for `fat_sectors`, TotSec32 the
/// volume's.
fn with_fat(mut image: SparseImage, first: u64, fat_sectors: u64, volume: u64, cluster: u8) -> SparseImage {
    let mut vbr = ffi::fat32_vbr(fat_sectors, cluster);
    vbr[32..36].copy_from_slice(&(volume as u32).to_le_bytes());
    image.set(first, &vbr);
    image
}

fn fat_first(old_root: bool, type_guid: [u8; 16], name: &str) -> SparseImage {
    let image = owner::fat_first(&json(), old_root, type_guid, name);
    with_fat(image, FAT_FIRST, OLD_FAT_SECTORS, NEW_FAT_SECTORS, 64)
}

fn report(s: &Scenario) {
    let table = matrix::run(s);
    eprint!("{table}");
    if let Ok(dir) = std::env::var("TEST_UNDECLARED_OUTPUTS_DIR") {
        std::fs::write(format!("{dir}/{}.txt", s.name), &table).unwrap();
    }
}

#[test]
fn the_kernels_root_type_and_name_are_these() {
    let source = std::fs::read_to_string(std::env::var("SWITCH_BLOCK_CPP").unwrap()).unwrap();
    assert!(source.contains("const char kPartitionName[] = \"iOS\";\n"));
    let at = source.find("const uint8_t kApfsType[16] = {").unwrap();
    let list = &source[at..][..source[at..].find("};").unwrap()];
    let bytes: Vec<u8> = list
        .split(['{', ',', ' ', '\n'])
        .filter_map(|t| t.strip_prefix("0x"))
        .map(|h| u8::from_str_radix(h, 16).unwrap())
        .collect();
    assert_eq!(bytes, APFS_TYPE);
    assert_eq!(ffi::apfs_type(), APFS_TYPE);
}

#[test]
fn before_any_install_the_kernel_finds_no_root_and_the_whole_fat() {
    let mut image = with_fat(owner::before(&json()), FAT_FIRST, OLD_FAT_SECTORS, OLD_FAT_SECTORS, 64);
    let root = ffi::root(&mut image).unwrap();
    assert_eq!(root.found, 0);
    let card = Card::open(&mut image).unwrap();
    assert!(card.roots().is_empty() && !card.torn());
    let userdata = card.entries()[owner::USERDATA as usize];
    let files = ffi::files(&mut image, userdata.first(), userdata.sectors());
    assert_eq!(
        files,
        Files::Volume {
            first: FAT_FIRST,
            count: OLD_FAT_SECTORS,
            exfat: false
        }
    );
}

#[test]
fn owner_fresh_install_into_the_appended_entry() {
    report(&Scenario {
        name: "owner-fresh",
        image: fat_first(false, LINUX_TYPE, "userdata"),
        choice: Some(owner::APPENDED),
        fat: (FAT_FIRST, NEW_FAT_SECTORS),
    });
}

#[test]
fn owner_moves_from_the_old_root_to_the_appended_entry() {
    report(&Scenario {
        name: "owner-move",
        image: fat_first(true, LINUX_TYPE, "iOS-setup"),
        choice: None,
        fat: (FAT_FIRST, NEW_FAT_SECTORS),
    });
}

#[test]
fn owner_reinstall_over_the_root() {
    let mut image = fat_first(false, LINUX_TYPE, "userdata");
    let mut card = Card::open(&mut image).unwrap();
    let plan = card.plan(Some(owner::APPENDED), NEED).unwrap();
    card.mark(&mut image, &plan).unwrap();
    card.finish(&mut image, &plan).unwrap();
    report(&Scenario {
        name: "owner-reinstall",
        image,
        choice: None,
        fat: (FAT_FIRST, NEW_FAT_SECTORS),
    });
}

/// A root in one primary array sector and the target in another: the demotion's sector lands first.
fn split_card(entry_size: u32) -> SparseImage {
    const SECTORS: u64 = 16 << 20;
    let image = tables::card(
        SECTORS,
        entry_size,
        8,
        &[
            (0, tables::entry(BASIC_DATA_TYPE, 1, "hos_data", 32768, 4 << 20)),
            (1, tables::entry(APFS_TYPE, 2, "iOS", 129 << 15, 2 << 20)),
            (6, tables::entry(LINUX_TYPE, 7, "userdata", 193 << 15, 4 << 20)),
        ],
    );
    with_fat(image, 32768, 4 << 20, 4 << 20, 4)
}

#[test]
fn demotion_lands_before_promotion_across_sectors() {
    for (name, size) in [("split-128", 128), ("split-256", 256)] {
        report(&Scenario {
            name,
            image: split_card(size),
            choice: Some(6),
            fat: (32768, 4 << 20),
        });
    }
}

/// The owner's card as a 1 TB sparse image file, through `StreamDevice::file`.
#[test]
fn owner_card_as_an_image_file() {
    let image = fat_first(true, LINUX_TYPE, "iOS-setup");
    let path = std::path::Path::new(&std::env::var("TEST_TMPDIR").unwrap()).join("owner.img");
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(&path)
        .unwrap();
    file.set_len(image.sectors * 512).unwrap();
    let mut dev = StreamDevice::file(file, 512).unwrap();
    for (lba, sector) in &image.data {
        switch_card::Device::write_sectors(&mut dev, *lba, &sector[..]).unwrap();
    }
    let mut card = Card::open(&mut dev).unwrap();
    let plan = card.plan(None, NEED).unwrap();
    assert_eq!(
        (plan.target.index, plan.others.clone()),
        (owner::APPENDED, vec![owner::USERDATA])
    );
    card.mark(&mut dev, &plan).unwrap();
    card.finish(&mut dev, &plan).unwrap();
    let root = ffi::root(&mut dev).unwrap();
    assert!(root.unique);
    assert_eq!((root.first, root.first + root.count), (NEW_FAT_END, OLD_FAT_END));
    let files = ffi::files(&mut dev, root.first, root.count);
    assert_eq!(
        files,
        Files::Volume {
            first: FAT_FIRST,
            count: NEW_FAT_SECTORS,
            exfat: false
        }
    );
    let reread = Card::open(&mut dev).unwrap();
    assert!(!reread.torn() && reread.roots() == vec![owner::APPENDED]);
    let demoted = reread.entries()[owner::USERDATA as usize];
    assert!(demoted.named("userdata") && demoted.type_guid() == LINUX_TYPE);
    drop(dev);
    std::fs::remove_file(path).unwrap();
}
