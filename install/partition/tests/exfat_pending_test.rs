// SPDX-License-Identifier: GPL-3.0-or-later
//! Pending-journal tamper and intervening-use refusal tests for exFAT and FAT32: every attack must return
//! `Error::Changed` with zero bytes written to the card.

use switch_card_testing::{Recorder, SparseImage};
use switch_partition::Error;
use switch_partition::journal::{self, Phase, Record, State};
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::cuts;
use switch_partition_testing::exfat::{Exfat, entry_set_checksum};
use switch_partition_testing::exfat_fixtures;
use switch_partition_testing::fat32::Fat32;
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use switch_partition_testing::{exfat_reader, macos, reader, scenario};

fn record_writes(base: &SparseImage) -> Vec<(u64, Vec<u8>)> {
    let mut done = base.clone();
    let events = {
        let mut rec = Recorder::new(&mut done);
        scenario::partition(&mut rec, SMALL_IOS);
        rec.events
    };
    cuts::writes(&events)
}

fn find_move_step_header_idx(base: &SparseImage, writes: &[(u64, Vec<u8>)]) -> (usize, u32, u32) {
    for (pos, (lba, _)) in writes.iter().enumerate() {
        if *lba != journal::FIRST {
            continue;
        }
        let mut st = cuts::state(base, &writes[..pos + 1]);
        if let State::Pending(h, step) = journal::read(&mut st).unwrap()
            && h.phase == Phase::Relocate
            && step.groups.len() >= 3
            && step
                .groups
                .last()
                .is_some_and(|g| g.iter().any(|r| matches!(r, Record::Free { .. })))
        {
            let dst = step.groups[0]
                .iter()
                .find_map(|r| match *r {
                    Record::Chain { first, .. } => Some(first),
                    _ => None,
                })
                .unwrap();
            let src = step
                .groups
                .last()
                .unwrap()
                .iter()
                .find_map(|r| match *r {
                    Record::Free { first, .. } => Some(first),
                    _ => None,
                })
                .unwrap();
            return (pos, src, dst);
        }
    }
    panic!("no Relocate move step found");
}

fn assert_changed_without_writes(image: &mut SparseImage, label: &str) {
    let snap = image.clone();
    let res = switch_partition::resume(image);
    assert!(
        matches!(res, Err(Error::Changed { .. })),
        "{label}: expected Err(Error::Changed), got {res:?}"
    );
    assert!(*image == snap, "{label}: refusal mutated card");
}

#[test]
fn c1_pending_move_new_file_in_dst_refused_without_writes() {
    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&base, vec![]);
        let src = cut + 10;
        v.file(&mut base, &mut root, "victim.bin", 512, &[src], 101, false);

        let all_writes = record_writes(&base);
        let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        v.fat_set(&mut cut_img, dst, 0xffff_ffff);
        let mut r = v.root_dir(&cut_img, vec![]);
        v.file(&mut cut_img, &mut r, "zelda_save.bin", 512, &[dst], 999, true);
        let expected = exfat_reader::list(&mut cut_img, FAT_FIRST, true);

        assert_changed_without_writes(&mut cut_img, "exfat stale FAT + 1-cluster NoFatChain in dst");
        assert_eq!(exfat_reader::list(&mut cut_img, FAT_FIRST, true), expected);
        macos::fsck_exfat(&cut_img, FAT_FIRST, SMALL_FAT, "c1-exp01a");
    }

    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&base, vec![]);
        let src = cut + 10;
        v.file(
            &mut base,
            &mut root,
            "victim4.bin",
            4 * 512,
            &[src, src + 1, src + 2, src + 3],
            202,
            false,
        );

        let all_writes = record_writes(&base);
        let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        let mut r = v.root_dir(&cut_img, vec![]);
        v.file(
            &mut cut_img,
            &mut r,
            "mario4.bin",
            4 * 512,
            &[dst, dst + 1, dst + 2, dst + 3],
            888,
            true,
        );
        let expected = exfat_reader::list(&mut cut_img, FAT_FIRST, true);

        assert_changed_without_writes(&mut cut_img, "exfat zero FAT + 4-cluster NoFatChain in dst");
        assert_eq!(exfat_reader::list(&mut cut_img, FAT_FIRST, true), expected);
        macos::fsck_exfat(&cut_img, FAT_FIRST, SMALL_FAT, "c1-exp01b");
    }

    {
        let mut base = cards::gpt(SMALL_SECTORS, SMALL_FAT);
        macos::format_fat32(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let f = Fat32::read(&base, FAT_FIRST);
        let cut = fixtures::cut_cluster(&f, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = f.root_dir(&base, vec![]);
        let src = cut + 10;
        f.file(&mut base, &mut root, "victim.bin", 512, &[src], 303);
        f.finish(&mut base);

        let all_writes = record_writes(&base);
        let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        let mut r = f.root_dir(&cut_img, vec![]);
        f.file(&mut cut_img, &mut r, "zelda.bin", 512, &[dst], 777);
        f.finish(&mut cut_img);
        let expected = reader::list(&mut cut_img, FAT_FIRST, 0);

        assert_changed_without_writes(&mut cut_img, "fat32 1-cluster new file in dst");
        assert_eq!(reader::list(&mut cut_img, FAT_FIRST, 0), expected);
        macos::fsck_fat32(&cut_img, FAT_FIRST, SMALL_FAT, "c1-exp01c");
    }
}

#[test]
fn c1_midchain_delete_and_reuse_refused_without_writes() {
    let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
    let v = Exfat::read(&base, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = v.root_dir(&base, vec![]);
    let pred = 500u32;
    let src = cut + 50;
    v.file(&mut base, &mut root, "chained.bin", 2 * 512, &[pred, src], 601, false);

    let all_writes = record_writes(&base);
    let (pos, found_src, _) = find_move_step_header_idx(&base, &all_writes);
    assert_eq!(found_src, src);

    let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
    let root_lba = v.lba(v.root);
    let mut rsec = cut_img.sector(root_lba);
    let mut i = 0;
    while i < 16 {
        if rsec[i * 32] == 0x85 {
            let sec = rsec[i * 32 + 1] as usize;
            let first = u32::from_le_bytes(rsec[(i + 1) * 32 + 20..(i + 1) * 32 + 24].try_into().unwrap());
            if first == pred {
                for k in 0..=sec {
                    rsec[(i + k) * 32] &= !0x80;
                }
                break;
            }
            i += 1 + sec;
        } else {
            i += 1;
        }
    }
    cut_img.set(root_lba, &rsec);
    v.bit_set(&mut cut_img, pred, false);
    v.bit_set(&mut cut_img, src, false);
    let mut r = v.root_dir(&cut_img, vec![]);
    v.file(&mut cut_img, &mut r, "reuse_pred.bin", 512, &[pred], 602, true);
    v.file(&mut cut_img, &mut r, "reuse_src.bin", 512, &[src], 603, true);
    let expected = exfat_reader::list(&mut cut_img, FAT_FIRST, true);

    assert_changed_without_writes(&mut cut_img, "mid-chain delete + NoFatChain reuse");
    assert_eq!(exfat_reader::list(&mut cut_img, FAT_FIRST, true), expected);
    macos::fsck_exfat(&cut_img, FAT_FIRST, SMALL_FAT, "c1-exp04");
}

#[test]
fn c1_temp_file_in_dst_and_inplace_edit_refused_without_writes() {
    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&base, vec![]);
        let src = cut + 10;
        v.file(&mut base, &mut root, "victim.bin", 512, &[src], 111, false);
        let orig_bytes = base.sector(v.lba(src));

        let all_writes = record_writes(&base);
        let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        cut_img.set(v.lba(dst), &[0xee; 512]);
        assert_changed_without_writes(&mut cut_img, "exfat temp create+delete in dst");
        assert_eq!(cut_img.sector(v.lba(src)), orig_bytes);
    }

    {
        let mut base = cards::gpt(SMALL_SECTORS, SMALL_FAT);
        macos::format_fat32(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let f = Fat32::read(&base, FAT_FIRST);
        let cut = fixtures::cut_cluster(&f, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = f.root_dir(&base, vec![]);
        let src = cut + 10;
        f.file(&mut base, &mut root, "victim.bin", 512, &[src], 222);
        let orig_bytes = base.sector(f.lba(src));

        let all_writes = record_writes(&base);
        let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        cut_img.set(f.lba(dst), &[0xdd; 512]);
        assert_changed_without_writes(&mut cut_img, "fat32 temp create+delete in dst");
        assert_eq!(cut_img.sector(f.lba(src)), orig_bytes);
    }

    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&base, vec![]);
        let pred = 500u32;
        let src = cut + 50;
        v.file(&mut base, &mut root, "save.bin", 2 * 512, &[pred, src], 333, false);

        let all_writes = record_writes(&base);
        let (pos, found_src, _) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
        cut_img.set(v.lba(src), &[0x99; 512]);
        let r_lba = v.lba(v.root);
        let mut rsec = cut_img.sector(r_lba);
        let fslot = (0..16).find(|&i| rsec[i * 32] == 0x85).unwrap();
        let slen = (1 + rsec[fslot * 32 + 1] as usize) * 32;
        rsec[fslot * 32 + 12] ^= 1;
        let sum = entry_set_checksum(&rsec[fslot * 32..fslot * 32 + slen]);
        rsec[fslot * 32 + 2..fslot * 32 + 4].copy_from_slice(&sum.to_le_bytes());
        cut_img.set(r_lba, &rsec);

        assert_changed_without_writes(&mut cut_img, "exfat mid-chain in-place edit");
        assert_eq!(cut_img.sector(v.lba(src)), [0x99; 512]);
    }
}

#[test]
fn c2_pending_free_window_and_shrink_tail_refused_without_writes() {
    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&base, vec![]);
        let src = cut + 10;
        v.file(&mut base, &mut root, "victim.bin", 512, &[src], 404, false);

        let all_writes = record_writes(&base);
        let (pos, found_src, _) = find_move_step_header_idx(&base, &all_writes);
        assert_eq!(found_src, src);

        let post_free = ((pos + 1)..all_writes.len())
            .find_map(|k| {
                let mut st = cuts::state(&base, &all_writes[..k]);
                (matches!(journal::read(&mut st).unwrap(), State::Pending(h, _) if h.phase == Phase::Relocate)
                    && !v.bit_get(&st, src)
                    && v.fat_get(&st, src) == 0)
                    .then_some(st)
            })
            .expect("expected post-Group-2 pre-finish cut");

        let mut drop_st = post_free.clone();
        v.fat_set(&mut drop_st, src, 0xffff_ffff);
        let mut r = v.root_dir(&drop_st, vec![]);
        v.file(&mut drop_st, &mut r, "new_nfc.bin", 512, &[src], 555, true);
        let expected_a = exfat_reader::list(&mut drop_st, FAT_FIRST, true);
        assert_changed_without_writes(&mut drop_st, "EXP02A Group 2 epoch-drop + NoFatChain in src");
        assert_eq!(exfat_reader::list(&mut drop_st, FAT_FIRST, true), expected_a);
        macos::fsck_exfat(&drop_st, FAT_FIRST, SMALL_FAT, "c2-exp02a");

        let mut st2 = post_free;
        let mut r = v.root_dir(&st2, vec![]);
        v.file(&mut st2, &mut r, "new_fat.bin", 512, &[src], 556, false);
        let expected_a2 = exfat_reader::list(&mut st2, FAT_FIRST, true);
        assert_changed_without_writes(&mut st2, "EXP02A2 post-Group-2 + FAT-chained in src");
        assert_eq!(exfat_reader::list(&mut st2, FAT_FIRST, true), expected_a2);
        macos::fsck_exfat(&st2, FAT_FIRST, SMALL_FAT, "c2-exp02a2");
    }

    {
        let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&base, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);

        let all_writes = record_writes(&base);
        let mut shrink_commit = None;
        let mut first_surplus = 0u32;
        let mut surplus_len = 0u32;
        for (k, (lba, _)) in all_writes.iter().enumerate() {
            if *lba == journal::FIRST {
                let mut st = cuts::state(&base, &all_writes[..k + 1]);
                if let State::Pending(h, step) = journal::read(&mut st).unwrap()
                    && h.phase == Phase::Shrink
                    && let Some((f, l)) = step.groups.last().unwrap().iter().find_map(|r| match *r {
                        Record::Free { first, len, .. } => Some((first, len)),
                        _ => None,
                    })
                {
                    shrink_commit = Some(k);
                    first_surplus = f;
                    surplus_len = l;
                    break;
                }
            }
        }
        let sk = shrink_commit.expect("expected Shrink commit with surplus bitmap clusters");

        let mut pre_boot = cuts::state(&base, &all_writes[..sk + 1]);
        let mut r = v.root_dir(&pre_boot, vec![]);
        v.file(&mut pre_boot, &mut r, "tail_save.bin", 512, &[cut + 20], 776, true);
        assert_changed_without_writes(&mut pre_boot, "Shrink pre-boot new file past cut");
        macos::fsck_exfat(&pre_boot, FAT_FIRST, SMALL_FAT, "c2-shrink-tail");

        let pre_finish = ((sk + 1)..all_writes.len())
            .rev()
            .find_map(|k| {
                let mut st = cuts::state(&base, &all_writes[..k]);
                (matches!(journal::read(&mut st).unwrap(), State::Pending(h, _) if h.phase == Phase::Shrink)
                    && !v.bit_get(&st, first_surplus)
                    && v.fat_get(&st, first_surplus) == 0)
                    .then_some(st)
            })
            .expect("expected post-Group-4 pre-finish Shrink cut");

        let clusters: Vec<u32> = (first_surplus..first_surplus + surplus_len).collect();

        let mut st_nfc = pre_finish.clone();
        for i in 0..surplus_len {
            let next = if i + 1 < surplus_len {
                first_surplus + i + 1
            } else {
                0xffff_ffff
            };
            v.fat_set(&mut st_nfc, first_surplus + i, next);
        }
        let mut r = v.root_dir(&st_nfc, vec![]);
        v.file(
            &mut st_nfc,
            &mut r,
            "nfc_save.bin",
            clusters.len() * 512,
            &clusters,
            778,
            true,
        );
        let expected_b2 = exfat_reader::list(&mut st_nfc, FAT_FIRST, true);
        assert_changed_without_writes(&mut st_nfc, "EXP02B2 Shrink Group 4 epoch-drop + NoFatChain");
        assert_eq!(exfat_reader::list(&mut st_nfc, FAT_FIRST, true), expected_b2);
        macos::fsck_exfat(&st_nfc, FAT_FIRST, SMALL_FAT, "c2-exp02b2");

        let mut st = pre_finish;
        let mut r = v.root_dir(&st, vec![]);
        v.file(
            &mut st,
            &mut r,
            "user_save.bin",
            clusters.len() * 512,
            &clusters,
            777,
            false,
        );
        let expected_b = exfat_reader::list(&mut st, FAT_FIRST, true);
        assert_changed_without_writes(&mut st, "EXP02B Shrink post-Group-4 + FAT-chained");
        assert_eq!(exfat_reader::list(&mut st, FAT_FIRST, true), expected_b);
        macos::fsck_exfat(&st, FAT_FIRST, SMALL_FAT, "c2-exp02b");
    }
}

#[test]
fn l3_fat32_corrupted_dotdot_before_group0_refused_without_writes() {
    let mut base = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut base, FAT_FIRST, SMALL_FAT, 1);
    let f = Fat32::read(&base, FAT_FIRST);
    let cut = fixtures::cut_cluster(&f, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = f.root_dir(&base, vec![]);
    let mut parent = f.mkdir(&mut base, &mut root, "parent", vec![cut + 10]);
    let child_clus = 500u32;
    f.mkdir(&mut base, &mut parent, "child", vec![child_clus]);
    f.finish(&mut base);

    let all_writes = record_writes(&base);
    let (pos, found_src, dst) = find_move_step_header_idx(&base, &all_writes);
    assert_eq!(found_src, cut + 10);

    let mut cut_img = cuts::state(&base, &all_writes[..pos + 1]);
    assert_eq!(f.get(&cut_img, dst), 0, "Group 0 must not have run yet");
    let child_lba = f.lba(child_clus);
    let mut csec = cut_img.sector(child_lba);
    let bogus = 999u32;
    csec[32 + 20..32 + 22].copy_from_slice(&((bogus >> 16) as u16).to_le_bytes());
    csec[32 + 26..32 + 28].copy_from_slice(&(bogus as u16).to_le_bytes());
    cut_img.set(child_lba, &csec);

    assert_changed_without_writes(&mut cut_img, "L3 FAT32 corrupted .. before Group 0");
    assert_eq!(f.get(&cut_img, dst), 0, "Group 0 FAT entry must remain untouched");
}
