// SPDX-License-Identifier: GPL-3.0-or-later
//! Regression tests for critical review findings (C1-C4).

use switch_card::{BASIC_DATA_TYPE, Card, LINUX_TYPE};
use switch_card_testing::{Recorder, SparseImage, tables};
use switch_partition::Error;
use switch_partition::journal::{self, Phase, Record, State};
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::cuts;
use switch_partition_testing::fat32::Fat32;
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, SMALL_SECTORS, Shape};
use switch_partition_testing::oracle::{self, Files};
use switch_partition_testing::{macos, reader, scenario};

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

fn multi_batch_card() -> (SparseImage, Fat32, u32) {
    let mut base = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut base, FAT_FIRST, SMALL_FAT, 1);
    let v = Fat32::read(&base, FAT_FIRST);
    let cut = fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = v.root_dir(&base, vec![]);
    let victim = cut + 10;
    v.file(&mut base, &mut root, "victim.bin", 512, &[victim], 42);
    let pieces: Vec<u32> = (0..500u32)
        .map(|i| if i % 2 == 0 { cut + 100 + 2 * i } else { 500 + 2 * i })
        .collect();
    v.file(&mut base, &mut root, "many.bin", pieces.len() * 512, &pieces, 99);
    v.finish(&mut base);
    (base, v, victim)
}

fn delete_file_by_cluster(image: &mut SparseImage, v: &Fat32, cluster: u32) {
    v.set(image, cluster, 0);
    let root_lba = v.lba(v.root);
    let mut rsec = image.sector(root_lba);
    let mut found = false;
    for slot in 0..16 {
        let e = &mut rsec[slot * 32..][..32];
        if e[0] != 0 && e[0] != 0xe5 && e[11] & 0x3f != 0x0f {
            let clus =
                (u32::from(u16::from_le_bytes([e[20], e[21]])) << 16) | u32::from(u16::from_le_bytes([e[26], e[27]]));
            if clus == cluster {
                e[0] = 0xe5;
                found = true;
            }
        }
    }
    assert!(found);
    for slot in (0..15).rev() {
        if rsec[slot * 32 + 11] & 0x3f == 0x0f && rsec[(slot + 1) * 32] == 0xe5 {
            rsec[slot * 32] = 0xe5;
        }
    }
    image.set(root_lba, &rsec);
}

#[test]
fn c1_finished_batch_header_closes_copy_window_for_new_and_deleted_files() {
    let (base, v, victim_cluster) = multi_batch_card();
    let mut done = base.clone();
    let events = {
        let mut rec = Recorder::new(&mut done);
        let (_, rep) = scenario::partition(&mut rec, SMALL_IOS);
        assert!(rep.batches >= 2);
        rec.events
    };

    let all_writes = cuts::writes(&events);
    let mut header_writes = 0;
    let mut before_finish_idx = None;
    let mut after_finish_idx = None;
    let mut before_batch2_body_idx = None;
    for (idx, (lba, _)) in all_writes.iter().enumerate() {
        if *lba == journal::FIRST {
            header_writes += 1;
            if header_writes == 3 {
                before_finish_idx = Some(idx);
                after_finish_idx = Some(idx + 1);
            }
        } else if header_writes == 3 && *lba == journal::FIRST + 1 {
            before_batch2_body_idx = Some(idx);
            break;
        }
    }
    let before_finish = before_finish_idx.unwrap();
    let after_finish = after_finish_idx.unwrap();
    let before_batch2_body = before_batch2_body_idx.unwrap();
    assert!(before_batch2_body > after_finish);

    let mid_pending = cuts::state(&base, &all_writes[..before_finish]);
    let new_victim = match journal::read(&mut mid_pending.clone()).unwrap() {
        State::Pending(h, step) => {
            assert_eq!(h.seq, 1);
            step.groups[0]
                .iter()
                .find_map(|r| match *r {
                    Record::Chain {
                        first, len: 1, last, ..
                    } if last >= 0x0fff_fff8 => Some(first),
                    _ => None,
                })
                .unwrap()
        }
        other => panic!("expected Pending(seq=1), got {other:?}"),
    };

    let mid_finished = cuts::state(&base, &all_writes[..before_batch2_body]);
    assert!(matches!(
        journal::read(&mut mid_finished.clone()).unwrap(),
        State::Finished(h) if h.seq == 2 && h.phase == Phase::Relocate
    ));

    let mut img_a = mid_finished.clone();
    let mut root_a = v.root_dir(&img_a, vec![]);
    v.file(&mut img_a, &mut root_a, "zelda_save.bin", 512, &[victim_cluster], 777);
    let expected_a = reader::list(&mut img_a, FAT_FIRST, 0);
    macos::fsck_fat32(&img_a, FAT_FIRST, SMALL_FAT, "c1a-before");
    let rep_a = switch_partition::resume(&mut img_a).unwrap().unwrap();
    assert!(rep_a.resumed);
    let fat_new_a = Card::open(&mut img_a).unwrap().entries()[0].sectors();
    for copy in [0, 1] {
        assert_eq!(reader::list(&mut img_a, FAT_FIRST, copy), expected_a);
    }
    macos::fsck_fat32(&img_a, FAT_FIRST, fat_new_a, "c1a-after");

    let mut img_b = mid_finished;
    delete_file_by_cluster(&mut img_b, &v, new_victim);
    let expected_b = reader::list(&mut img_b, FAT_FIRST, 0);
    macos::fsck_fat32(&img_b, FAT_FIRST, SMALL_FAT, "c1b-before");
    switch_partition::resume(&mut img_b).unwrap().unwrap();
    assert_eq!(v.get(&img_b, new_victim), 0);
    let fat_new_b = Card::open(&mut img_b).unwrap().entries()[0].sectors();
    for copy in [0, 1] {
        assert_eq!(reader::list(&mut img_b, FAT_FIRST, copy), expected_b);
    }
    macos::fsck_fat32(&img_b, FAT_FIRST, fat_new_b, "c1b-after");

    let mut img_c = mid_pending;
    delete_file_by_cluster(&mut img_c, &v, new_victim);
    let err_c = switch_partition::resume(&mut img_c).expect_err("deleted entry refused");
    assert!(matches!(err_c, Error::Changed { .. }));
    assert_eq!(v.get(&img_c, new_victim), 0);
}

#[test]
fn c1_stale_journal_refused_on_reformat_repartition_and_no_room() {
    let (base, v, _) = multi_batch_card();

    let mut img_reformat = base.clone();
    let plan = switch_partition::survey(&mut img_reformat)
        .unwrap()
        .plan(SMALL_IOS, 0)
        .unwrap();
    journal::begin(&mut img_reformat, &plan).unwrap();
    macos::format_fat32(&mut img_reformat, FAT_FIRST, SMALL_FAT, 1);
    let mut boot = img_reformat.sector(FAT_FIRST);
    let new_serial = plan.fingerprint.vol_serial.wrapping_add(1);
    boot[67..71].copy_from_slice(&new_serial.to_le_bytes());
    img_reformat.set(FAT_FIRST, &boot);
    img_reformat.set(FAT_FIRST + 6, &boot);
    let snap = img_reformat.clone();
    assert!(matches!(
        switch_partition::resume(&mut img_reformat),
        Err(Error::WrongCard(_))
    ));
    assert!(img_reformat == snap);

    let mut img_repart = base.clone();
    journal::begin(&mut img_repart, &plan).unwrap();
    let mut mbr = img_repart.sector(0);
    mbr[0] ^= 0xff;
    img_repart.set(0, &mbr);
    let snap = img_repart.clone();
    assert!(matches!(
        switch_partition::resume(&mut img_repart),
        Err(Error::WrongCard(_))
    ));
    assert!(img_repart == snap);

    let mut img_full = base.clone();
    journal::begin(&mut img_full, &plan).unwrap();
    let cut = fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let free_below: Vec<u32> = (2..cut).filter(|&c| v.get(&img_full, c) == 0).collect();
    let mut root = v.root_dir(&img_full, vec![]);
    let take = free_below.len() - 50;
    v.file_zero(&mut img_full, &mut root, "fill.bin", take * 512, &free_below[..take]);
    let snap = img_full.clone();
    assert!(matches!(
        switch_partition::resume(&mut img_full),
        Err(Error::NoRoom { .. })
    ));
    assert!(img_full == snap);

    let mut done_img = base.clone();
    let mut rec = Recorder::new(&mut done_img);
    scenario::partition(&mut rec, SMALL_IOS);
    let writes = cuts::writes(&rec.events);
    let last_header_idx = writes
        .iter()
        .rposition(|(lba, data)| *lba == journal::FIRST && data.iter().any(|&b| b != 0))
        .unwrap();
    let mut after_tables_finish = cuts::state(&base, &writes[..last_header_idx + 1]);
    assert!(matches!(
        journal::read(&mut after_tables_finish).unwrap(),
        State::Finished(h) if h.phase == Phase::Tables
    ));
    let mut bad_mbr = after_tables_finish.sector(0);
    bad_mbr[458..462].copy_from_slice(&(SMALL_FAT as u32).to_le_bytes());
    after_tables_finish.set(0, &bad_mbr);
    assert!(matches!(
        switch_partition::resume(&mut after_tables_finish),
        Err(Error::WrongCard(_))
    ));
}

#[test]
fn c2_standard_128_entry_and_256_byte_entry_gpt() {
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, SMALL_FAT);

    let mut img128 = tables::card(SMALL_SECTORS, 128, 128, &[(0, fat)]);
    fixtures::fill(&mut img128, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    let before = reader::list(&mut img128, FAT_FIRST, 0);
    let (plan, _) = scenario::partition(&mut img128, SMALL_IOS);
    assert_eq!(plan.entry_index, 1);
    scenario::check_done(&mut img128, &plan, &before, 0, "c2-128-empty");
    assert_eq!(Card::open(&mut img128).unwrap().entries().len(), 128);
    macos::fsck_fat32(&img128, FAT_FIRST, plan.fat_new_sectors, "c2-128-empty");

    let last128 = tables::entry(LINUX_TYPE, 2, "extra", FAT_FIRST + SMALL_FAT, 1024);
    let mut full128 = tables::card(SMALL_SECTORS, 128, 128, &[(0, fat), (127, last128)]);
    fixtures::fill(&mut full128, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    assert!(matches!(
        refused(&mut full128, SMALL_IOS),
        Error::Card(switch_card::Error::Header {
            backup: false,
            why: "entry count"
        })
    ));

    let mut img256 = tables::card(SMALL_SECTORS, 256, 64, &[(0, fat)]);
    fixtures::fill(&mut img256, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    let before256 = reader::list(&mut img256, FAT_FIRST, 0);
    let (plan256, _) = scenario::partition(&mut img256, SMALL_IOS);
    assert_eq!(plan256.entry_index, 1);
    scenario::check_done(&mut img256, &plan256, &before256, 0, "c2-256-empty");

    let mut full256 = tables::card(SMALL_SECTORS, 256, 64, &[(0, fat), (63, last128)]);
    fixtures::fill(&mut full256, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    assert!(matches!(
        refused(&mut full256, SMALL_IOS),
        Error::Card(switch_card::Error::Header {
            backup: false,
            why: "array placement"
        })
    ));
}

#[test]
fn c3_entry_size_256_backup_garbage_stays_clean() {
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, SMALL_FAT);
    let p1 = tables::entry(LINUX_TYPE, 2, "p1", FAT_FIRST + SMALL_FAT, 256);
    let p2 = tables::entry(LINUX_TYPE, 3, "p2", FAT_FIRST + SMALL_FAT + 256, 256);
    let p3 = tables::entry(LINUX_TYPE, 4, "p3", FAT_FIRST + SMALL_FAT + 512, 256);
    let mut image = tables::card(SMALL_SECTORS, 256, 4, &[(0, fat), (1, p1), (2, p2), (3, p3)]);
    fixtures::fill(&mut image, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, &SIMPLE);
    let backup_array = SMALL_SECTORS - 33;
    let mut sec = image.sector(backup_array + 2);
    sec[128..256].fill(0xab);
    image.set(backup_array + 2, &sec);

    let before = reader::list(&mut image, FAT_FIRST, 0);
    let (plan, _) = scenario::partition(&mut image, SMALL_IOS);
    assert_eq!(plan.entry_index, 4);
    scenario::check_done(&mut image, &plan, &before, 0, "c3-256-garbage");
    assert_eq!(
        oracle::files(&mut image, plan.entry_first, plan.entry_sectors),
        Files::Volume {
            first: FAT_FIRST,
            count: plan.fat_new_sectors,
            exfat: false,
        }
    );
}

#[test]
fn c4_backup_fsinfo_bounded_and_overlap_refused() {
    let mut image = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut image, FAT_FIRST, SMALL_FAT, 1);
    let mut boot = image.sector(FAT_FIRST);
    let fat_sz = u32::from_le_bytes(boot[36..40].try_into().unwrap()) as u64;
    let fsi = image.sector(FAT_FIRST + 1);
    image.set(FAT_FIRST + 15, &fsi);
    boot[48..50].copy_from_slice(&15u16.to_le_bytes());
    boot[50..52].copy_from_slice(&20u16.to_le_bytes());
    image.set(FAT_FIRST, &boot);
    image.set(FAT_FIRST + 20, &boot);

    let fat0_before = image.sector(FAT_FIRST + 35);
    let (plan, events) = {
        let mut rec = Recorder::new(&mut image);
        let (plan, _) = scenario::partition(&mut rec, SMALL_IOS);
        (plan, rec.events)
    };
    let touched_fat0_3 = cuts::writes(&events)
        .iter()
        .any(|(lba, data)| *lba <= FAT_FIRST + 35 && FAT_FIRST + 35 < *lba + (data.len() / 512) as u64);
    assert!(!touched_fat0_3);
    assert_eq!(image.sector(FAT_FIRST + 35), fat0_before);
    assert_eq!(image.sector(FAT_FIRST + 35), image.sector(FAT_FIRST + 35 + fat_sz));
    assert_eq!(&image.sector(FAT_FIRST + 15)[488..496], &[0xff; 8]);
    assert_eq!(
        Card::open(&mut image).unwrap().entries()[0].sectors(),
        plan.fat_new_sectors
    );

    let mut overlap = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut overlap, FAT_FIRST, SMALL_FAT, 1);
    let mut b = overlap.sector(FAT_FIRST);
    b[48..50].copy_from_slice(&6u16.to_le_bytes());
    b[50..52].copy_from_slice(&6u16.to_le_bytes());
    overlap.set(FAT_FIRST, &b);
    overlap.set(FAT_FIRST + 6, &b);
    assert!(matches!(refused(&mut overlap, SMALL_IOS), Error::Volume(_)));
}
