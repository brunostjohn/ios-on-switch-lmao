// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT C1 crash-resume tamper tests and Shrink backup-before-main barrier ordering.

use switch_card::Card;
use switch_card_testing::{Event, Recorder, SparseImage};
use switch_partition::Error;
use switch_partition::journal::{self, Phase, Record, State};
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::cuts;
use switch_partition_testing::exfat::Exfat;
use switch_partition_testing::exfat_fixtures;
use switch_partition_testing::fixtures::{SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use switch_partition_testing::{exfat_reader, macos, scenario};

fn recompute_boot_regions(image: &mut SparseImage) {
    let mut main = image.read_bytes(FAT_FIRST, 12);
    let mut sum = 0u32;
    for (i, &b) in main[..11 * 512].iter().enumerate() {
        if i == 106 || i == 107 || i == 112 {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    let bytes = sum.to_le_bytes();
    for slot in main[11 * 512..12 * 512].as_chunks_mut::<4>().0 {
        slot.copy_from_slice(&bytes);
    }
    image.set(FAT_FIRST, &main);
    image.set(FAT_FIRST + 12, &main);
}

fn delete_root_file_by_cluster(image: &mut SparseImage, v: &Exfat, cluster: u32) {
    let root_lba = v.lba(v.root);
    let mut rsec = image.sector(root_lba);
    let mut found = false;
    let mut i = 0;
    while i < 16 {
        if rsec[i * 32] == 0x85 {
            let sec = rsec[i * 32 + 1] as usize;
            let first = u32::from_le_bytes(rsec[(i + 1) * 32 + 20..(i + 1) * 32 + 24].try_into().unwrap());
            if first == cluster {
                for k in 0..=sec {
                    rsec[(i + k) * 32] &= !0x80;
                }
                found = true;
                break;
            }
            i += 1 + sec;
        } else {
            i += 1;
        }
    }
    assert!(found);
    image.set(root_lba, &rsec);
    v.bit_set(image, cluster, false);
}

#[test]
fn c1_resume_tamper_and_stale_journal_refusals() {
    let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
    let v = Exfat::read(&base, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = v.root_dir(&base, vec![]);
    let victim = cut + 10;
    v.file(&mut base, &mut root, "victim.bin", 512, &[victim], 42, false);
    let nfc_c = cut + 20;
    v.file(&mut base, &mut root, "nfc.bin", 512, &[nfc_c], 43, true);
    let pieces: Vec<u32> = (0..300u32)
        .map(|i| if i % 2 == 0 { cut + 100 + 2 * i } else { 500 + 2 * i })
        .collect();
    v.file(&mut base, &mut root, "many.bin", pieces.len() * 512, &pieces, 99, false);

    let mut done = base.clone();
    let events = {
        let mut rec = Recorder::new(&mut done);
        let (_, rep) = scenario::partition(&mut rec, SMALL_IOS);
        assert!(rep.batches >= 3);
        rec.events
    };
    let all_writes = cuts::writes(&events);
    let header_positions: Vec<usize> = all_writes
        .iter()
        .enumerate()
        .filter(|(_, (lba, _))| *lba == journal::FIRST)
        .map(|(i, _)| i)
        .collect();

    let mut pending_conv = cuts::state(&base, &all_writes[..header_positions[1] + 1]);
    assert!(matches!(
        journal::read(&mut pending_conv).unwrap(),
        State::Pending(h, _) if h.seq == 1
    ));
    delete_root_file_by_cluster(&mut pending_conv, &v, nfc_c);
    let snap = pending_conv.clone();
    assert!(matches!(
        switch_partition::resume(&mut pending_conv),
        Err(Error::Changed { .. })
    ));
    assert!(pending_conv == snap);

    let mut pending_move = cuts::state(&base, &all_writes[..header_positions[3] + 1]);
    let new_victim = match journal::read(&mut pending_move).unwrap() {
        State::Pending(h, step) => {
            assert_eq!(h.seq, 3);
            step.groups[0]
                .iter()
                .find_map(|r| match *r {
                    Record::Chain {
                        first,
                        len: 1,
                        last: 0xffff_ffff,
                        ..
                    } => Some(first),
                    _ => None,
                })
                .unwrap()
        }
        other => panic!("expected Pending(seq=3), got {other:?}"),
    };
    delete_root_file_by_cluster(&mut pending_move, &v, victim);
    let snap = pending_move.clone();
    assert!(matches!(
        switch_partition::resume(&mut pending_move),
        Err(Error::Changed { .. })
    ));
    assert!(pending_move == snap);
    assert!(!v.bit_get(&pending_move, new_victim));

    // Mid-chain-only batch: exFAT delete may clear bitmap bits without zeroing FAT.
    let mut pending_mid = cuts::state(&base, &all_writes[..header_positions[5] + 1]);
    let mid_pred = match journal::read(&mut pending_mid).unwrap() {
        State::Pending(_, step) => step.groups[1]
            .iter()
            .find_map(|r| match *r {
                Record::Fat { index, .. } => Some(index),
                _ => None,
            })
            .expect("batch 2 moves mid-chain clusters"),
        other => panic!("expected Pending mid-chain batch, got {other:?}"),
    };
    v.bit_set(&mut pending_mid, mid_pred, false);
    let snap_mid = pending_mid.clone();
    assert!(matches!(
        switch_partition::resume(&mut pending_mid),
        Err(Error::Changed { .. })
    ));
    assert!(pending_mid == snap_mid);

    let mut boot_epochs = Vec::new();
    let mut epoch = 0usize;
    for ev in &events {
        match ev {
            Event::Barrier => epoch += 1,
            Event::Write(lba, data) => {
                let end = *lba + (data.len() / 512) as u64;
                for (off, label) in [(0u64, "main-0"), (11, "main-11"), (12, "bk-12"), (23, "bk-23")] {
                    if *lba <= FAT_FIRST + off && FAT_FIRST + off < end {
                        boot_epochs.push((epoch, label));
                    }
                }
            }
        }
    }
    let bk_max = boot_epochs
        .iter()
        .filter(|(_, l)| l.starts_with("bk-"))
        .map(|(e, _)| *e)
        .max()
        .unwrap();
    let main_min = boot_epochs
        .iter()
        .filter(|(_, l)| l.starts_with("main-"))
        .map(|(e, _)| *e)
        .min()
        .unwrap();
    assert!(
        bk_max < main_min,
        "backup boot region must barrier before main: {boot_epochs:?}"
    );

    let mid_finished = cuts::state(&base, &all_writes[..header_positions[4] + 1]);
    assert!(matches!(
        journal::read(&mut mid_finished.clone()).unwrap(),
        State::Finished(h) if h.seq == 4 && h.phase == Phase::Relocate
    ));

    let mut img_new_file = mid_finished.clone();
    let mut r = v.root_dir(&img_new_file, vec![]);
    v.file(&mut img_new_file, &mut r, "zelda_save.bin", 512, &[victim], 777, true);
    let expected = exfat_reader::list(&mut img_new_file, FAT_FIRST, true);
    switch_partition::resume(&mut img_new_file).unwrap().unwrap();
    let fat_new = Card::open(&mut img_new_file).unwrap().entries()[0].sectors();
    assert_eq!(exfat_reader::list(&mut img_new_file, FAT_FIRST, true), expected);
    macos::fsck_exfat(&img_new_file, FAT_FIRST, fat_new, "c1-exfat-new-file");

    let mut img_del = mid_finished;
    delete_root_file_by_cluster(&mut img_del, &v, new_victim);
    let expected_del = exfat_reader::list(&mut img_del, FAT_FIRST, true);
    switch_partition::resume(&mut img_del).unwrap().unwrap();
    let fat_new_del = Card::open(&mut img_del).unwrap().entries()[0].sectors();
    assert_eq!(exfat_reader::list(&mut img_del, FAT_FIRST, true), expected_del);
    macos::fsck_exfat(&img_del, FAT_FIRST, fat_new_del, "c1-exfat-del-file");

    let plan = switch_partition::survey(&mut base.clone())
        .unwrap()
        .plan(SMALL_IOS, 0)
        .unwrap();

    let mut to_fat32 = base.clone();
    journal::begin(&mut to_fat32, &plan).unwrap();
    macos::format_fat32(&mut to_fat32, FAT_FIRST, SMALL_FAT, 1);
    assert!(matches!(
        switch_partition::resume(&mut to_fat32),
        Err(Error::WrongCard(_))
    ));

    let mut new_serial = base.clone();
    journal::begin(&mut new_serial, &plan).unwrap();
    let mut b = new_serial.sector(FAT_FIRST);
    let ser = plan.fingerprint.vol_serial.wrapping_add(1);
    b[100..104].copy_from_slice(&ser.to_le_bytes());
    new_serial.set(FAT_FIRST, &b);
    recompute_boot_regions(&mut new_serial);
    assert!(matches!(
        switch_partition::resume(&mut new_serial),
        Err(Error::WrongCard(_))
    ));

    let mut repart = base.clone();
    journal::begin(&mut repart, &plan).unwrap();
    let mut mbr = repart.sector(0);
    mbr[0] ^= 0xff;
    repart.set(0, &mbr);
    assert!(matches!(
        switch_partition::resume(&mut repart),
        Err(Error::WrongCard(_))
    ));

    let mut full = base;
    journal::begin(&mut full, &plan).unwrap();
    let free_below: Vec<u32> = (2..cut).filter(|&c| !v.bit_get(&full, c)).collect();
    let mut fr = v.root_dir(&full, vec![]);
    let take = free_below.len() - 10;
    v.file_zero(&mut full, &mut fr, "fill.bin", take * 512, &free_below[..take], false);
    assert!(matches!(switch_partition::resume(&mut full), Err(Error::NoRoom { .. })));
}
