// SPDX-License-Identifier: GPL-3.0-or-later
//! Regression tests for high review findings (H1-H6).

use std::io::Cursor;

use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, Card, Device, LINUX_TYPE, StreamDevice};
use switch_card_testing::{SparseImage, tables};
use switch_partition::Error;
use switch_partition_testing::cards::FAT_FIRST;
use switch_partition_testing::fat32::Fat32;
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, SMALL_SECTORS, Shape};

const SIMPLE: Shape = Shape {
    root_in_cut: false,
    full_tail: false,
    fragments: 0,
    filler: false,
};

fn refused(image: &mut SparseImage, ios_bytes: u64) -> Error {
    let original = image.clone();
    let err = match switch_partition::survey(image) {
        Err(e) => e,
        Ok(survey) => match survey.plan(ios_bytes, 0) {
            Err(e) => e,
            Ok(plan) => switch_partition::run(image, &plan).expect_err("expected refusal"),
        },
    };
    assert!(*image == original, "refusal mutated card: {err}");
    err
}

#[test]
fn h1_torn_gpt_refused_before_any_write() {
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, SMALL_FAT);
    let other = tables::entry(LINUX_TYPE, 2, "userdata", FAT_FIRST + SMALL_FAT, 1024);
    let mut image = tables::card(SMALL_SECTORS, 128, 2, &[(0, fat), (1, other)]);
    fixtures::fill(&mut image, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);

    let backup_array = SMALL_SECTORS - 33;
    let mut bsec = image.sector(backup_array);
    bsec[128..256].copy_from_slice(&other.retyped(LINUX_TYPE, "iOS-setup").0);
    image.set(backup_array, &bsec);
    let crc = crc32fast::hash(&bsec[..256]);
    let mut bhdr = image.sector(SMALL_SECTORS - 1);
    bhdr[88..92].copy_from_slice(&crc.to_le_bytes());
    bhdr[16..20].fill(0);
    let hcrc = crc32fast::hash(&bhdr[..92]);
    bhdr[16..20].copy_from_slice(&hcrc.to_le_bytes());
    image.set(SMALL_SECTORS - 1, &bhdr);

    assert!(Card::open(&mut image).unwrap().torn());
    assert!(matches!(
        refused(&mut image, SMALL_IOS),
        Error::Card(switch_card::Error::CopiesDiffer(_))
    ));
}

#[test]
fn h2_stale_or_mismatched_backup_boot_refused() {
    let base = fixtures::small(&SIMPLE);

    let mut zeroed = base.clone();
    zeroed.set(FAT_FIRST + 6, &[0u8; 512]);
    assert!(matches!(refused(&mut zeroed, SMALL_IOS), Error::Damaged(_)));

    let mut bad_tot = base.clone();
    let mut b = bad_tot.sector(FAT_FIRST + 6);
    let tot = u32::from_le_bytes(b[32..36].try_into().unwrap()) - 1;
    b[32..36].copy_from_slice(&tot.to_le_bytes());
    bad_tot.set(FAT_FIRST + 6, &b);
    assert!(matches!(refused(&mut bad_tot, SMALL_IOS), Error::Damaged(_)));

    let mut bad_root = base;
    let mut b = bad_root.sector(FAT_FIRST + 6);
    let root = u32::from_le_bytes(b[44..48].try_into().unwrap()) + 1;
    b[44..48].copy_from_slice(&root.to_le_bytes());
    bad_root.set(FAT_FIRST + 6, &b);
    assert!(matches!(refused(&mut bad_root, SMALL_IOS), Error::Damaged(_)));
}

#[test]
fn h3_dirty_fat_and_fat01_mirror_mismatch_refused() {
    let base = fixtures::small(&SIMPLE);
    let v = Fat32::read(&base, FAT_FIRST);

    let mut nt_dirty = base.clone();
    for lba in [FAT_FIRST, FAT_FIRST + 6] {
        let mut b = nt_dirty.sector(lba);
        b[65] |= 0x01;
        nt_dirty.set(lba, &b);
    }
    assert!(matches!(refused(&mut nt_dirty, SMALL_IOS), Error::Damaged(_)));

    let mut unclean = base.clone();
    let f1 = v.get(&unclean, 1);
    v.set(&mut unclean, 1, f1 & !0x0800_0000);
    assert!(matches!(refused(&mut unclean, SMALL_IOS), Error::Damaged(_)));

    let mut io_err = base.clone();
    v.set(&mut io_err, 1, f1 & !0x0400_0000);
    assert!(matches!(refused(&mut io_err, SMALL_IOS), Error::Damaged(_)));

    let mut mismatch = base;
    let fat1_lba = FAT_FIRST + v.reserved + v.fat_sectors;
    let mut s = mismatch.sector(fat1_lba);
    s[0..8].copy_from_slice(&[0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]);
    mismatch.set(fat1_lba, &s);
    assert!(matches!(refused(&mut mismatch, SMALL_IOS), Error::Damaged(_)));
}

#[test]
fn h4_bad_cluster_refused_anywhere() {
    let base = fixtures::small(&SIMPLE);
    let v = Fat32::read(&base, FAT_FIRST);
    let cut = fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);

    let mut past = base.clone();
    let bad_past = v.free(&past, cut + 11_000, 1)[0];
    v.set(&mut past, bad_past, 0x0fff_fff7);
    assert!(matches!(refused(&mut past, SMALL_IOS), Error::Damaged(_)));

    let mut below = base;
    let bad_below = v.free(&below, 50, 1)[0];
    v.set(&mut below, bad_below, 0x0fff_fff7);
    assert!(matches!(refused(&mut below, SMALL_IOS), Error::Damaged(_)));
}

#[test]
fn h5_odd_sector_size_and_small_device_refused() {
    struct FourK {
        sectors: u64,
    }
    impl Device for FourK {
        fn sector_size(&self) -> u32 {
            4096
        }
        fn sector_count(&self) -> u64 {
            self.sectors
        }
        fn read_sectors(&mut self, _lba: u64, _buf: &mut [u8]) -> std::io::Result<()> {
            panic!("4K device must be rejected before reading LBA 32752");
        }
        fn write_sectors(&mut self, _lba: u64, _buf: &[u8]) -> std::io::Result<()> {
            panic!("4K device must never be written");
        }
        fn barrier(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    let mut d = FourK { sectors: 40_000 };
    assert!(matches!(
        switch_partition::survey(&mut d),
        Err(Error::Card(switch_card::Error::SectorSize(4096)))
    ));
    assert!(matches!(
        switch_partition::pending(&mut d),
        Err(Error::Card(switch_card::Error::SectorSize(4096)))
    ));
    assert!(matches!(
        switch_partition::resume(&mut d),
        Err(Error::Card(switch_card::Error::SectorSize(4096)))
    ));

    let mut small = StreamDevice::new(Cursor::new(vec![0u8; 4096 * 512]), 512, 4096, |_| Ok(()));
    assert!(matches!(
        switch_partition::survey(&mut small),
        Err(Error::Card(switch_card::Error::TooSmall(4096)))
    ));
    assert!(matches!(
        switch_partition::pending(&mut small),
        Err(Error::Card(switch_card::Error::TooSmall(4096)))
    ));
    assert!(matches!(
        switch_partition::resume(&mut small),
        Err(Error::Card(switch_card::Error::TooSmall(4096)))
    ));
}

#[test]
fn h6_multiple_roots_and_foreign_root_refused() {
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, SMALL_FAT);
    let r1 = tables::entry(APFS_TYPE, 2, "iOS", FAT_FIRST + SMALL_FAT, 1024);
    let r2 = tables::entry(APFS_TYPE, 3, "iOS", FAT_FIRST + SMALL_FAT + 1024, 1024);
    let mut two_roots = tables::card(SMALL_SECTORS + 4096, 128, 3, &[(0, fat), (1, r1), (2, r2)]);
    fixtures::fill(
        &mut two_roots,
        SMALL_FAT,
        1,
        SMALL_SECTORS + 4096 - 33,
        SMALL_IOS,
        &SIMPLE,
    );
    assert!(matches!(
        refused(&mut two_roots, SMALL_IOS),
        Error::Card(switch_card::Error::Ambiguous(_))
    ));

    let bad_fat = tables::entry(BASIC_DATA_TYPE, 1, "iOS", FAT_FIRST, SMALL_FAT);
    let mut foreign = tables::card(SMALL_SECTORS, 128, 1, &[(0, bad_fat)]);
    fixtures::fill(&mut foreign, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    assert!(matches!(
        refused(&mut foreign, SMALL_IOS),
        Error::Card(switch_card::Error::ForeignRoot(0))
    ));
}
