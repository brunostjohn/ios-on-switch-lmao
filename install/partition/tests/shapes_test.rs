// SPDX-License-Identifier: GPL-3.0-or-later
//! Partition mode on GPT cards: a free tail, a used one, a full one, the root directory in the cut, a run through an
//! image file for throughput, and what is refused before any write.

use std::fs::OpenOptions;

use switch_card::{Device, StreamDevice};
use switch_card_testing::SparseImage;
use switch_partition::Error;
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, SMALL_SECTORS, Shape};
use switch_partition_testing::{macos, reader, scenario};

fn shape(root_in_cut: bool, full_tail: bool) -> SparseImage {
    fixtures::small(&Shape {
        root_in_cut,
        full_tail,
        fragments: 0,
        filler: true,
    })
}

fn shrink(name: &str, mut image: SparseImage) -> switch_partition::Report {
    let before = reader::list(&mut image, FAT_FIRST, 0);
    let roots = scenario::roots(&mut image);
    macos::fsck_fat32(&image, FAT_FIRST, SMALL_FAT, &format!("{name}-before"));
    let (plan, report) = scenario::partition(&mut image, SMALL_IOS);
    scenario::check_done(&mut image, &plan, &before, roots, name);
    let fsck = macos::fsck_fat32(&image, FAT_FIRST, plan.fat_new_sectors, name);
    eprintln!(
        "{name}: {} files, moved {} KiB in {} batches; fsck_msdos -n:\n{fsck}",
        before.len(),
        report.moved_bytes >> 10,
        report.batches
    );
    report
}

#[test]
fn a_free_tail_moves_nothing() {
    let mut image = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut image, FAT_FIRST, SMALL_FAT, 1);
    let report = shrink("free-tail", image);
    assert_eq!((report.moved_bytes, report.batches), (0, 0));
}

/// A volume already shorter than its new partition keeps its length: the shrink never grows `TotSec32`.
#[test]
fn a_volume_shorter_than_its_new_partition_keeps_its_length() {
    let mut image = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut image, FAT_FIRST, 300_000, 1);
    let report = shrink("short-volume", image.clone());
    assert_eq!((report.moved_bytes, report.batches), (0, 0));
    let (plan, _) = scenario::partition(&mut image, SMALL_IOS);
    assert!(plan.fat_new_sectors > 300_000);
    let boot = image.sector(FAT_FIRST);
    assert_eq!(u32::from_le_bytes(boot[32..36].try_into().unwrap()), 300_000);
}

#[test]
fn files_and_directories_past_the_cut_move_below_it() {
    let report = shrink("used-tail", shape(false, false));
    assert!(report.moved_bytes > 2 << 20);
}

#[test]
fn a_tail_full_to_the_last_cluster_moves_into_the_holes_below() {
    let report = shrink("full-tail", shape(false, true));
    assert!(report.moved_bytes > 70 << 20 && report.batches > 1);
}

#[test]
fn the_root_directory_itself_can_be_in_the_cut() {
    shrink("root-in-cut", shape(true, false));
}

/// The full tail again, through `StreamDevice::file` on a sparse image file: its barrier is a real `fsync`.
#[test]
fn throughput_through_an_image_file() {
    let image = shape(false, true);
    let path = macos::scratch("throughput.img");
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
        dev.write_sectors(*lba, &sector[..]).unwrap();
    }
    dev.barrier().unwrap();
    let before = reader::list(&mut dev, FAT_FIRST, 0);
    let (plan, report) = scenario::partition(&mut dev, SMALL_IOS);
    assert!(reader::list(&mut dev, FAT_FIRST, 0) == before);
    assert_eq!(plan.fat_new_sectors, plan.entry_first - FAT_FIRST);
    eprintln!(
        "image file: moved {} MiB in {} batches, {:.2} s copying, {:.1} MiB/s",
        report.moved_bytes >> 20,
        report.batches,
        report.copy_seconds,
        report.throughput / f64::from(1 << 20)
    );
    drop(dev);
    std::fs::remove_file(path).unwrap();
}

fn refused(image: &mut SparseImage, ios_bytes: u64) -> Error {
    let original = image.clone();
    let err = match switch_partition::survey(image) {
        Err(e) => e,
        Ok(survey) => match survey.plan(ios_bytes, 0) {
            Err(e) => e,
            Ok(plan) => switch_partition::run(image, &plan).expect_err("the run should refuse"),
        },
    };
    assert!(*image == original, "a refusal wrote to the card: {err}");
    err
}

#[test]
fn refusals_leave_the_card_untouched() {
    let mut image = shape(false, false);
    assert!(matches!(refused(&mut image, SMALL_IOS + 512), Error::Size { .. }));
    assert!(matches!(
        refused(&mut image, 160 << 20),
        Error::Size { .. } | Error::NoRoom { .. }
    ));

    let mut journal = image.clone();
    journal.set(switch_partition::journal::FIRST + 5, &[1; 512]);
    assert!(matches!(refused(&mut journal, SMALL_IOS), Error::Journal(_)));

    let mut lost = image.clone();
    let v = switch_partition_testing::fat32::Fat32::read(&lost, FAT_FIRST);
    let cut = fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    v.set(&mut lost, cut + 11_000, 0x0fff_ffff);
    assert!(matches!(refused(&mut lost, SMALL_IOS), Error::Damaged(_)));

    let mut copies = image.clone();
    let fat2 = FAT_FIRST + v.reserved + v.fat_sectors + 3;
    copies.set(fat2, &[0x77; 512]);
    assert!(matches!(refused(&mut copies, SMALL_IOS), Error::Damaged(_)));

    let mut exfat = image.clone();
    let mut boot = exfat.sector(FAT_FIRST);
    boot[3..11].copy_from_slice(b"EXFAT   ");
    exfat.set(FAT_FIRST, &boot);
    switch_card_testing::tables::mbr_slot(&mut exfat, 0, 0x07, FAT_FIRST, SMALL_FAT);
    assert!(matches!(refused(&mut exfat, SMALL_IOS), Error::Volume(_)));

    let mut twice = image.clone();
    scenario::partition(&mut twice, SMALL_IOS);
    assert!(matches!(refused(&mut twice, SMALL_IOS), Error::AlreadyPartitioned(1)));
}
