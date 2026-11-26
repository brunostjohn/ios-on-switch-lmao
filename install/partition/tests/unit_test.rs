// SPDX-License-Identifier: GPL-3.0-or-later
//! The journal's on-card format and its reading after a cut, and the size picker's rules.

use switch_card::Device;
use switch_card_testing::SparseImage;
use switch_partition::journal::{self, FieldKind, Phase, Record, State, Step};
use switch_partition::{Fingerprint, FsKind, Limits, Plan, minimum_ios_bytes};

const GIB: u64 = 1 << 30;

fn plan() -> Plan {
    Plan {
        card_sectors: 1 << 30,
        fat_first: 32768,
        fat_sectors: 1 << 29,
        fat_new_sectors: 1 << 28,
        entry_first: 32768 + (1 << 28),
        entry_sectors: 64 << 21,
        entry_index: 10,
        entry_guid: [7; 16],
        fs: FsKind::Fat32,
        new_gpt: Some([9; 16]),
        fingerprint: Fingerprint {
            vol_serial: 0x1234_5678,
            vol_boot_sig: 0x29,
            vol_total: 1 << 29,
            boot_crc: 0xdead_beef,
            table_crc: 0xcafe_babe,
        },
    }
}

fn every_record() -> Step {
    Step {
        groups: vec![
            vec![
                Record::Chain {
                    first: 5,
                    len: 3,
                    last: 0x0fff_ffff,
                    loose: true,
                },
                Record::Zero { lba: 999, count: 31 },
            ],
            vec![
                Record::Fat {
                    index: 4,
                    old: 100,
                    new: 5,
                },
                Record::Field {
                    lba: 1 << 33,
                    offset: 480,
                    kind: FieldKind::Fat32Cluster,
                    old: 100,
                    new: 5,
                },
                Record::Field {
                    lba: 3,
                    offset: 2,
                    kind: FieldKind::Le16,
                    old: 0xbeef,
                    new: 0xcafe,
                },
                Record::DotDot {
                    old: 100,
                    new: 5,
                    root: false,
                },
            ],
            vec![
                Record::Free {
                    first: 100,
                    len: 3,
                    last: 0x0fff_fff8,
                },
                Record::Sector {
                    lba: 0,
                    old: 0x1234_5678,
                    new: Box::new([0xa5; 512]),
                },
            ],
        ],
    }
}

#[test]
fn a_step_reads_back_as_written() {
    let mut image = SparseImage::new(1 << 16);
    let step = every_record();
    journal::begin(&mut image, &plan()).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Finished(h) => assert_eq!((h.seq, h.phase, h.plan), (0, Phase::Relocate, plan())),
        other => panic!("{other:?}"),
    }
    journal::commit(&mut image, 1, Phase::Shrink, &plan(), &step).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Pending(h, s) => assert_eq!((h.seq, h.phase, h.plan, s), (1, Phase::Shrink, plan(), step)),
        other => panic!("{other:?}"),
    }
    journal::finish(&mut image, 2, Phase::Shrink, &plan()).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Finished(h) => assert_eq!((h.seq, h.phase, h.plan), (2, Phase::Shrink, plan())),
        other => panic!("{other:?}"),
    }
}

/// The next step's body went down, its header didn't: the step in the header finished.
#[test]
fn a_body_the_header_does_not_match_is_a_finished_step() {
    let mut image = SparseImage::new(1 << 16);
    journal::begin(&mut image, &plan()).unwrap();
    journal::commit(&mut image, 1, Phase::Relocate, &plan(), &every_record()).unwrap();
    let mut body = image.read_bytes(journal::FIRST + 1, journal::SECTORS - 1);
    body[..8].copy_from_slice(&2u64.to_le_bytes());
    image.set(journal::FIRST + 1, &body);
    assert!(matches!(journal::read(&mut image).unwrap(), State::Finished(h) if h.seq == 1));
    journal::clear(&mut image).unwrap();
    assert_eq!(journal::read(&mut image).unwrap(), State::Empty);
}

#[test]
fn data_that_is_not_a_journal_is_refused() {
    let mut image = SparseImage::new(1 << 16);
    image.write_sectors(journal::FIRST + 9, &[1; 512]).unwrap();
    assert!(journal::read(&mut image).is_err());
    let mut image = SparseImage::new(1 << 16);
    journal::begin(&mut image, &plan()).unwrap();
    let mut header = image.sector(journal::FIRST);
    header[200] ^= 1;
    image.set(journal::FIRST, &header);
    assert!(journal::read(&mut image).is_err());
}

#[test]
fn the_picker_never_goes_under_16_gib_or_the_image_plus_4() {
    assert_eq!(minimum_ios_bytes(0), 16 * GIB);
    assert_eq!(minimum_ios_bytes(12 * GIB + GIB / 2), 16 * GIB + GIB / 2);
    assert_eq!(minimum_ios_bytes(20 * GIB + 1), 24 * GIB + (16 << 20));
    let roomy = Limits::new(5 * GIB, 300 * GIB);
    assert_eq!((roomy.min, roomy.default, roomy.max), (16 * GIB, 64 * GIB, 300 * GIB));
    let big_image = Limits::new(70 * GIB, 300 * GIB);
    assert_eq!(big_image.default, 74 * GIB);
    let small_card = Limits::new(5 * GIB, 40 * GIB + 5);
    assert_eq!((small_card.default, small_card.max), (40 * GIB, 40 * GIB));
    assert!(!Limits::new(5 * GIB, 10 * GIB).possible());
}
