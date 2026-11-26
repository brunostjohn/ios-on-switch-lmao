// SPDX-License-Identifier: GPL-3.0-or-later
//! The owner's 1 TB card (install/testdata/switch-card-1tb.json) with an 825 GiB FAT32 in 32 KiB clusters: the
//! 64 GiB carve lands where the kernel's layout model (sd_layout_test.cpp) puts the FAT's new end, and an install
//! into the new entry then boots.

use switch_card::{APFS_TYPE, Card, LINUX_TYPE};
use switch_card_testing::{SparseImage, owner};
use switch_partition::DEFAULT_IOS_BYTES;
use switch_partition_testing::cards::FAT_FIRST;
use switch_partition_testing::fixtures::{self, Shape};
use switch_partition_testing::oracle::{self, Files};
use switch_partition_testing::{macos, reader, scenario};

fn json() -> String {
    std::env::var("SWITCH_CARD_JSON").unwrap()
}

fn fresh(old_root: bool) -> SparseImage {
    let mut image = if old_root {
        owner::old_layout(&json())
    } else {
        owner::before(&json())
    };
    macos::format_fat32(&mut image, FAT_FIRST, owner::OLD_FAT_SECTORS, 64);
    image
}

/// The new entry's end: the last 16 MiB boundary in the old FAT. The kernel's model ends entry 10 at the old FAT's
/// end instead (15 MiB on), which its root rule and Files take the same way.
const ENTRY_END: u64 = owner::OLD_FAT_END / 0x8000 * 0x8000;

fn partition(name: &str, mut image: SparseImage) -> SparseImage {
    let lba0 = image.sector(0);
    let lba1 = image.sector(1);
    let before = reader::list(&mut image, FAT_FIRST, 0);
    let roots = scenario::roots(&mut image);
    let (plan, report) = scenario::partition(&mut image, DEFAULT_IOS_BYTES);
    assert_eq!(
        plan.fat_new_sectors,
        owner::NEW_FAT_SECTORS,
        "the model's new FAT length"
    );
    assert_eq!(plan.entry_first, owner::NEW_FAT_END, "the model's new FAT end");
    assert_eq!(plan.entry_index, owner::APPENDED);
    assert_eq!(plan.entry_first + plan.entry_sectors, ENTRY_END);
    scenario::check_done(&mut image, &plan, &before, roots, name);
    let (now0, now1) = (image.sector(0), image.sector(1));
    let changed = |a: &[u8; 512], b: &[u8; 512]| (0..512).filter(|&i| a[i] != b[i]).collect::<Vec<usize>>();
    assert!(
        changed(&lba0, &now0).iter().all(|i| (458..462).contains(i)),
        "LBA 0 beyond slot 0's length"
    );
    assert!(
        changed(&lba1, &now1)
            .iter()
            .all(|i| (16..20).contains(i) || (80..84).contains(i) || (88..92).contains(i)),
        "LBA 1 beyond the entry count and CRCs"
    );
    let fsck = macos::fsck_fat32(&image, FAT_FIRST, plan.fat_new_sectors, name);
    eprintln!(
        "{name}: {} files, moved {} KiB; fsck_msdos -n:\n{fsck}",
        before.len(),
        report.moved_bytes >> 10
    );
    image
}

/// switch_card's install on the partitioned card: entry 10 becomes the one root, any old one is demoted.
fn install(image: &mut SparseImage, demoted: &[u32]) {
    let mut card = Card::open(image).unwrap();
    let plan = card.plan(None, 1 << 30).unwrap();
    assert_eq!(
        (plan.target.index, plan.others.clone()),
        (owner::APPENDED, demoted.to_vec())
    );
    card.mark(image, &plan).unwrap();
    card.finish(image, &plan).unwrap();
    let root = oracle::root(image).unwrap();
    assert!(root.unique && root.first == owner::NEW_FAT_END && root.first + root.count == ENTRY_END);
    assert_eq!(
        oracle::files(image, root.first, root.count),
        Files::Volume {
            first: FAT_FIRST,
            count: owner::NEW_FAT_SECTORS,
            exfat: false
        }
    );
    let card = Card::open(image).unwrap();
    assert_eq!(card.roots(), vec![owner::APPENDED]);
    assert_eq!(card.entries()[owner::APPENDED as usize].type_guid(), APFS_TYPE);
    for &d in demoted {
        let e = card.entries()[d as usize];
        assert!(e.named("userdata") && e.type_guid() == LINUX_TYPE);
    }
}

#[test]
fn a_fresh_card_then_an_install_into_entry_10() {
    let mut image = partition("owner-fresh", fresh(false));
    install(&mut image, &[]);
}

#[test]
fn the_old_layout_keeps_booting_entry_8_until_the_install_demotes_it() {
    let mut image = partition("owner-old-layout", fresh(true));
    let root = oracle::root(&mut image).unwrap();
    assert!(root.unique && root.first == owner::USERDATA_FIRST);
    install(&mut image, &[owner::USERDATA]);
}

#[test]
fn files_past_the_cut_on_the_owners_card() {
    let mut image = owner::before(&json());
    let shape = Shape {
        root_in_cut: true,
        full_tail: false,
        fragments: 40,
        filler: false,
    };
    fixtures::fill(
        &mut image,
        owner::OLD_FAT_SECTORS,
        64,
        owner::SECTORS - 2048,
        DEFAULT_IOS_BYTES,
        &shape,
    );
    let mut image = partition("owner-used", image);
    install(&mut image, &[]);
}
