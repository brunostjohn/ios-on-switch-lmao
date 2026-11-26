// SPDX-License-Identifier: GPL-3.0-or-later
//! Targeted power-cut matrices for tricky layouts: Phase::Shrink intermediate cuts, already-short volumes,
//! cluster-straddling entry sets, and allocation bitmap entries in root tail clusters.

use switch_card_testing::Recorder;
use switch_partition::journal::{self, Phase, State};
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::cuts;
use switch_partition_testing::exfat::Exfat;
use switch_partition_testing::exfat_fixtures;
use switch_partition_testing::fixtures::{SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use switch_partition_testing::{macos, scenario};

#[test]
fn phase_shrink_cuts_short_volumes_same_clusters_and_cleartozero() {
    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let mut done = base.clone();
        let events = {
            let mut rec = Recorder::new(&mut done);
            scenario::partition(&mut rec, SMALL_IOS);
            rec.events
        };
        let all_writes = cuts::writes(&events);
        let mut shrink_start = 0;
        let mut shrink_end = 0;
        for (k, (lba, _)) in all_writes.iter().enumerate() {
            if *lba == journal::FIRST {
                let mut st = cuts::state(&base, &all_writes[..k + 1]);
                match journal::read(&mut st).unwrap() {
                    State::Pending(h, _) if h.phase == Phase::Shrink => shrink_start = k + 1,
                    State::Finished(h) if h.phase == Phase::Shrink => {
                        shrink_end = k;
                        break;
                    }
                    _ => {}
                }
            }
        }
        assert!(shrink_start > 0 && shrink_end > shrink_start);
        for k in shrink_start..=shrink_end {
            let mut rec = cuts::state(&base, &all_writes[..k]);
            switch_partition::resume(&mut rec).unwrap().unwrap();
            assert_eq!(cuts::first_difference(&rec, &done), None, "shrink cut k={k}");
        }
    }

    {
        let mut short = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut short, FAT_FIRST, 300_000, 1);
        scenario::matrix("exfat-short-vol", &short, SMALL_IOS, true, 10);
    }

    {
        let fat_new = (FAT_FIRST + SMALL_FAT) / 0x8000 * 0x8000 - SMALL_IOS / 512 - FAT_FIRST;
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, fat_new + 30, 64);
        let v_before = Exfat::read(&img, FAT_FIRST);
        let (plan, _) = scenario::partition(&mut img, SMALL_IOS);
        let v_after = Exfat::read(&img, FAT_FIRST);
        assert_eq!(v_before.clusters, v_after.clusters);
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp05c");
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 8);
        for off in [0u64, 12] {
            let mut b = img.sector(FAT_FIRST + off);
            b[106..108].copy_from_slice(&0x0008u16.to_le_bytes());
            img.set(FAT_FIRST + off, &b);
        }
        let (plan, _) = scenario::partition(&mut img, SMALL_IOS);
        let flags_after = u16::from_le_bytes([img.sector(FAT_FIRST)[106], img.sector(FAT_FIRST)[107]]);
        assert_eq!(flags_after, 0);
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp05d");
    }

    {
        let mut short_fat32 = cards::gpt(SMALL_SECTORS, SMALL_FAT);
        macos::format_fat32(&mut short_fat32, FAT_FIRST, 300_000, 1);
        scenario::matrix("fat32-short-vol", &short_fat32, SMALL_IOS, true, 10);
    }
}

#[test]
fn cluster_straddling_entry_set_and_bitmap_in_root_tail_cut_matrices() {
    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&img, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&img, vec![]);
        let mut sub = v.mkdir(&mut img, &mut root, "straddle-dir", vec![cut + 500, 600], false);
        for i in 0..5 {
            v.file(&mut img, &mut sub, &format!("p{i}.txt"), 0, &[], 1 + i as u64, true);
        }
        assert_eq!(v.slot_in_sector(&sub), 15);
        v.file(&mut img, &mut sub, "cross.bin", 512, &[cut + 510], 99, false);
        scenario::matrix("exp06c-cluster-straddle", &img, SMALL_IOS, true, 12);
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&img, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let tail = cut + 50;
        let r0_lba = v.lba(v.root);
        let mut r0 = img.sector(r0_lba);
        let bm_slot = (0..16).find(|&i| r0[i * 32] == 0x81).unwrap();
        let bm_entry: [u8; 32] = r0[bm_slot * 32..(bm_slot + 1) * 32].try_into().unwrap();
        r0[bm_slot * 32] = 0x01;
        for s in 0..16 {
            if r0[s * 32] == 0x00 {
                r0[s * 32] = 0x05;
            }
        }
        img.set(r0_lba, &r0);
        let mut r1 = [0u8; 512];
        r1[..32].copy_from_slice(&bm_entry);
        img.set(v.lba(tail), &r1);
        v.bit_set(&mut img, tail, true);
        v.fat_set(&mut img, v.root, tail);
        v.fat_set(&mut img, tail, 0xffff_ffff);
        scenario::matrix("exp06f-bm-in-root-tail", &img, SMALL_IOS, true, 12);
    }
}
