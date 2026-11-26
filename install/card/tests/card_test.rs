// SPDX-License-Identifier: GPL-3.0-or-later
//! What `Card` refuses and chooses, on synthetic cards and the owner's.

use std::io::Cursor;

use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, Card, Error, LINUX_TYPE, Role, StreamDevice};
use switch_card_testing::owner::{self, USERDATA, USERDATA_FIRST};
use switch_card_testing::{SparseImage, tables};

const SECTORS: u64 = 16 << 20;
const GIB: u64 = 1 << 30;

fn json() -> String {
    std::env::var("SWITCH_CARD_JSON").unwrap()
}

fn card(extra: &[(u32, switch_card::Entry)]) -> SparseImage {
    let mut entries = vec![(0, tables::entry(BASIC_DATA_TYPE, 1, "hos_data", 32768, 4 << 20))];
    entries.extend_from_slice(extra);
    tables::card(SECTORS, 128, 8, &entries)
}

fn userdata(index: u32, tag: u8, first: u64) -> (u32, switch_card::Entry) {
    (index, tables::entry(LINUX_TYPE, tag, "userdata", first, 2 << 20))
}

fn err(image: &mut SparseImage) -> Error {
    Card::open(image).err().expect("refused")
}

#[test]
fn refuses_a_card_without_a_gpt() {
    let mut image = card(&[]);
    let mut mbr = image.sector(0);
    mbr[462..478].fill(0);
    image.set(0, &mbr);
    assert!(matches!(err(&mut image), Error::NoGpt));
}

#[test]
fn refuses_sectors_other_than_512_bytes() {
    let mut dev = StreamDevice::new(Cursor::new(vec![0u8; 1 << 20]), 4096, 256, |_| Ok(()));
    assert!(matches!(Card::open(&mut dev), Err(Error::SectorSize(4096))));
}

#[test]
fn refuses_entries_the_strict_scan_refuses() {
    let mut image = tables::card(SECTORS, 136, 8, &[]);
    assert!(matches!(err(&mut image), Error::EntrySize(136)));
    let mut image = tables::card(
        SECTORS,
        512,
        8,
        &[(0, tables::entry(BASIC_DATA_TYPE, 1, "hos_data", 32768, 1 << 20))],
    );
    assert!(Card::open(&mut image).is_ok());
}

#[test]
fn refuses_a_damaged_header() {
    let mut image = card(&[]);
    let mut header = image.sector(1);
    header[40] ^= 1;
    image.set(1, &header);
    assert!(matches!(
        err(&mut image),
        Error::Header {
            backup: false,
            why: "header CRC"
        }
    ));
}

#[test]
fn refuses_copies_that_differ_beyond_a_type_and_name() {
    let mut image = card(&[userdata(1, 2, 129 << 15)]);
    let backup_array = SECTORS - 33;
    let mut sector = image.sector(backup_array);
    sector[128 + 32] ^= 0x80;
    image.set(backup_array, &sector);
    assert!(matches!(err(&mut image), Error::CopiesDiffer(_)));
}

#[test]
fn refuses_copies_where_neither_matches_its_crc() {
    let mut image = card(&[userdata(1, 2, 129 << 15)]);
    for lba in [2, SECTORS - 33] {
        let mut sector = image.sector(lba);
        sector[128 + 56] = b'X';
        image.set(lba, &sector);
    }
    assert!(matches!(
        err(&mut image),
        Error::CopiesDiffer("neither array matches its header's CRC")
    ));
}

#[test]
fn refuses_overlapping_entries() {
    let mut image = card(&[userdata(1, 2, 129 << 15), userdata(2, 3, (129 << 15) + 8)]);
    assert!(matches!(err(&mut image), Error::Entry { index: 2, .. }));
}

#[test]
fn chooses_the_unfinished_install_then_the_root_then_the_one_fresh_partition() {
    let setup = (3, tables::entry(LINUX_TYPE, 4, "iOS-setup", 257 << 15, 2 << 20));
    let root = (2, tables::entry(APFS_TYPE, 3, "iOS", 193 << 15, 2 << 20));
    let fresh = userdata(1, 2, 129 << 15);
    let pick = |entries: &[(u32, switch_card::Entry)]| Card::open(&mut card(entries)).unwrap().target(None);
    assert_eq!(pick(&[fresh, root, setup]).unwrap().index, 3);
    assert_eq!(pick(&[fresh, root]).unwrap().role, Role::Root);
    assert_eq!(pick(&[fresh]).unwrap().index, 1);
    assert!(matches!(pick(&[]), Err(Error::NoTarget)));
    let second = userdata(4, 5, 321 << 15);
    assert!(matches!(pick(&[fresh, second]), Err(Error::Ambiguous(v)) if v == [1, 4]));
}

#[test]
fn never_targets_the_fat_or_emummc() {
    let mut owner = owner::before(&json());
    let card = Card::open(&mut owner).unwrap();
    assert!(matches!(card.target(Some(0)), Err(Error::NotATarget(0))));
    assert!(matches!(card.target(Some(9)), Err(Error::NotATarget(9))));
    assert!(matches!(card.target(None), Err(Error::Ambiguous(v)) if v == [1, USERDATA]));
}

/// The owner's Android userdata starts 1 MiB past a 16 MiB boundary and boots as the root today, so install mode
/// takes it as it is.
#[test]
fn plans_the_owners_root_where_it_starts() {
    let mut image = owner::old_layout(&json());
    let card = Card::open(&mut image).unwrap();
    assert_eq!(card.roots(), vec![USERDATA]);
    assert_ne!(USERDATA_FIRST % 32768, 0);
    let plan = card.plan(None, GIB).unwrap();
    assert_eq!(plan.target.index, USERDATA);
}

#[test]
fn refuses_a_target_too_small_or_out_of_reach() {
    let mut image = card(&[userdata(1, 2, 129 << 15)]);
    let card = Card::open(&mut image).unwrap();
    assert!(matches!(card.plan(None, GIB), Ok(_)));
    assert!(matches!(
        card.plan(None, GIB + 1),
        Err(Error::TooSmallForImage { index: 1, .. })
    ));

    let big = 1u64 << 33;
    let mut image = tables::card(
        big,
        128,
        4,
        &[
            (0, tables::entry(BASIC_DATA_TYPE, 1, "hos_data", 32768, 4 << 20)),
            (1, tables::entry(LINUX_TYPE, 2, "userdata", 1 << 32, 2 << 20)),
        ],
    );
    let card = Card::open(&mut image).unwrap();
    assert!(matches!(card.plan(None, GIB), Err(Error::Unaddressable { index: 1 })));
}

#[test]
fn a_second_run_writes_nothing() {
    let mut image = card(&[userdata(1, 2, 129 << 15)]);
    let mut card = Card::open(&mut image).unwrap();
    let plan = card.plan(None, GIB).unwrap();
    assert_eq!(card.mark(&mut image, &plan).unwrap(), 4);
    assert_eq!(card.mark(&mut image, &plan).unwrap(), 0);
    assert_eq!(card.finish(&mut image, &plan).unwrap(), 4);
    let mut again = Card::open(&mut image).unwrap();
    let plan = again.plan(None, GIB).unwrap();
    assert_eq!(plan.target.role, Role::Root);
    assert_eq!(again.finish(&mut image, &plan).unwrap(), 0);
}

#[test]
fn the_boot_volume_is_mbr_slot_0_mirrored_by_gpt_index_0() {
    let mut image = card(&[]);
    assert_eq!(Card::open(&mut image).unwrap().boot_volume().unwrap(), (32768, 4 << 20));
    tables::mbr_slot(&mut image, 0, 0x0c, 32768, (4 << 20) - 8);
    assert!(matches!(
        Card::open(&mut image).unwrap().boot_volume(),
        Err(Error::NoBootVolume(_))
    ));
    tables::mbr_slot(&mut image, 0, 0x83, 32768, 4 << 20);
    assert!(matches!(
        Card::open(&mut image).unwrap().boot_volume(),
        Err(Error::NoBootVolume(_))
    ));
}

/// A FAT or data partition named `iOS` counts as a root for the kernel, but demoting it would rename the FAT.
#[test]
fn refuses_to_demote_a_fat_or_data_partition_named_ios() {
    let named_fat = tables::entry(BASIC_DATA_TYPE, 1, "iOS", 32768, 4 << 20);
    let mut image = tables::card(SECTORS, 128, 8, &[(0, named_fat), userdata(1, 2, 129 << 15)]);
    let read = Card::open(&mut image).unwrap();
    assert_eq!(read.roots(), vec![0]);
    assert!(matches!(read.plan(None, GIB), Err(Error::ForeignRoot(0))));

    let data = (2, tables::entry(BASIC_DATA_TYPE, 3, "iOS", 193 << 15, 2 << 20));
    let mut image = card(&[userdata(1, 2, 129 << 15), data]);
    let read = Card::open(&mut image).unwrap();
    assert!(matches!(read.plan(Some(1), GIB), Err(Error::ForeignRoot(2))));
}
