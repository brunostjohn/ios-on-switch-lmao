// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT refusal tests: dirty flags, bad boot/backup checksums, TexFAT, bad/lost/cross-linked clusters, malformed
//! entry sets, system files past cut, and wrong MBR slot type.

use switch_card_testing::{SparseImage, tables};
use switch_partition::Error;
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::exfat::{Exfat, entry_set_checksum};
use switch_partition_testing::exfat_fixtures::{self, ExfatShape};
use switch_partition_testing::fixtures::{SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use switch_partition_testing::macos;

const SIMPLE: ExfatShape = ExfatShape {
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

#[test]
fn boot_region_flags_checksum_backup_and_mbr_type_refusals() {
    let base = exfat_fixtures::small_exfat(&SIMPLE);

    for (flag, expect_damaged) in [(0x0002u16, true), (0x0004, true), (0x0001, false), (0x0010, false)] {
        let mut img = base.clone();
        for off in [0u64, 12] {
            let mut b = img.sector(FAT_FIRST + off);
            b[106..108].copy_from_slice(&flag.to_le_bytes());
            img.set(FAT_FIRST + off, &b);
        }
        let err = refused(&mut img, SMALL_IOS);
        if expect_damaged {
            assert!(matches!(err, Error::Damaged(_)), "flag {flag:#x}: {err}");
        } else {
            assert!(matches!(err, Error::Volume(_)), "flag {flag:#x}: {err}");
        }
    }

    for (at, val) in [(110usize, 2u8), (105, 2), (108, 12), (20, 1)] {
        let mut img = base.clone();
        let mut b = img.sector(FAT_FIRST);
        b[at] = val;
        img.set(FAT_FIRST, &b);
        recompute_boot_regions(&mut img);
        assert!(matches!(refused(&mut img, SMALL_IOS), Error::Volume(_)));
    }

    let mut bad_main_ck = base.clone();
    let mut ck = bad_main_ck.sector(FAT_FIRST + 11);
    ck[0] ^= 1;
    bad_main_ck.set(FAT_FIRST + 11, &ck);
    bad_main_ck.set(FAT_FIRST + 23, &ck);
    assert!(matches!(refused(&mut bad_main_ck, SMALL_IOS), Error::Damaged(_)));

    let mut bad_bk = base.clone();
    bad_bk.set(FAT_FIRST + 12, &[0u8; 512]);
    assert!(matches!(refused(&mut bad_bk, SMALL_IOS), Error::Damaged(_)));

    let mut bad_bk_ck = base.clone();
    let mut ck = bad_bk_ck.sector(FAT_FIRST + 23);
    ck[4] ^= 0xff;
    bad_bk_ck.set(FAT_FIRST + 23, &ck);
    assert!(matches!(refused(&mut bad_bk_ck, SMALL_IOS), Error::Damaged(_)));

    let mut bad_mbr = base;
    tables::mbr_slot(&mut bad_mbr, 0, 0x0b, FAT_FIRST, SMALL_FAT);
    assert!(matches!(refused(&mut bad_mbr, SMALL_IOS), Error::Mbr(_)));
}

#[test]
fn fat_bitmap_upcase_and_tree_integrity_refusals() {
    let base = exfat_fixtures::small_exfat(&SIMPLE);
    let v = Exfat::read(&base, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);

    let mut bad_fat0 = base.clone();
    v.fat_set(&mut bad_fat0, 0, 0);
    assert!(matches!(refused(&mut bad_fat0, SMALL_IOS), Error::Damaged(_)));

    let mut bad_fat1 = base.clone();
    v.fat_set(&mut bad_fat1, 1, 0xffff_fff0);
    assert!(matches!(refused(&mut bad_fat1, SMALL_IOS), Error::Damaged(_)));

    for start in [500u32, cut + 11_000] {
        let mut img = base.clone();
        let c = v.free(&img, start, 1)[0];
        v.fat_set(&mut img, c, 0xffff_fff7);
        assert!(matches!(refused(&mut img, SMALL_IOS), Error::Damaged(_)));
    }

    for start in [500u32, cut + 11_000] {
        let mut img = base.clone();
        let c = v.free(&img, start, 1)[0];
        v.bit_set(&mut img, c, true);
        assert!(matches!(refused(&mut img, SMALL_IOS), Error::Damaged(_)));
    }

    let root_lba = v.lba(v.root);
    let rsec = base.sector(root_lba);
    let bm_slot = (0..16).find(|&s| rsec[s * 32] == 0x81).unwrap();
    let up_slot = (0..16).find(|&s| rsec[s * 32] == 0x82).unwrap();

    let mut bm_flags = base.clone();
    let mut s = rsec;
    s[bm_slot * 32 + 1] = 1;
    bm_flags.set(root_lba, &s);
    assert!(matches!(refused(&mut bm_flags, SMALL_IOS), Error::Volume(_)));

    let mut bm_short = base.clone();
    let mut s = rsec;
    s[bm_slot * 32 + 24..bm_slot * 32 + 32].copy_from_slice(&10u64.to_le_bytes());
    bm_short.set(root_lba, &s);
    assert!(matches!(refused(&mut bm_short, SMALL_IOS), Error::Damaged(_)));

    let mut bm_missing = base.clone();
    let mut s = rsec;
    s[bm_slot * 32] = 0x01;
    bm_missing.set(root_lba, &s);
    assert!(matches!(refused(&mut bm_missing, SMALL_IOS), Error::Damaged(_)));

    let mut up_missing = base.clone();
    let mut s = rsec;
    s[up_slot * 32] = 0x02;
    up_missing.set(root_lba, &s);
    assert!(matches!(refused(&mut up_missing, SMALL_IOS), Error::Damaged(_)));

    let mut up_bad_sum = base.clone();
    let mut s = rsec;
    s[up_slot * 32 + 4] ^= 0xff;
    up_bad_sum.set(root_lba, &s);
    assert!(matches!(refused(&mut up_bad_sum, SMALL_IOS), Error::Damaged(_)));

    let mut fresh = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut fresh, FAT_FIRST, SMALL_FAT, 1);
    let fv = Exfat::read(&fresh, FAT_FIRST);
    let mut froot = fv.root_dir(&fresh, vec![]);
    let free = fv.free(&fresh, 500, 4);
    fv.file(&mut fresh, &mut froot, "test.bin", 1024, &free[..2], 7, false);

    let mut cycle = fresh.clone();
    fv.fat_set(&mut cycle, free[1], free[0]);
    let err = refused(&mut cycle, SMALL_IOS);
    assert!(matches!(err, Error::Damaged(ref msg) if msg.contains("loops")), "{err}");

    let mut clear_bit = fresh.clone();
    fv.bit_set(&mut clear_bit, free[1], false);
    assert!(matches!(refused(&mut clear_bit, SMALL_IOS), Error::Damaged(_)));

    let fr_lba = fv.lba(fv.root);
    let fr_sec = fresh.sector(fr_lba);
    let f_slot = (0..16).find(|&i| fr_sec[i * 32] == 0x85).unwrap();
    let set_len = (1 + fr_sec[f_slot * 32 + 1] as usize) * 32;

    let mut bad_sum = fresh.clone();
    let mut s = fr_sec;
    s[f_slot * 32 + 2] ^= 1;
    bad_sum.set(fr_lba, &s);
    assert!(matches!(refused(&mut bad_sum, SMALL_IOS), Error::Damaged(_)));

    let mut bad_vdl = fresh.clone();
    let mut s = fr_sec;
    s[(f_slot + 1) * 32 + 8..(f_slot + 1) * 32 + 16].copy_from_slice(&2048u64.to_le_bytes());
    let new_sum = entry_set_checksum(&s[f_slot * 32..f_slot * 32 + set_len]);
    s[f_slot * 32 + 2..f_slot * 32 + 4].copy_from_slice(&new_sum.to_le_bytes());
    bad_vdl.set(fr_lba, &s);
    assert!(matches!(refused(&mut bad_vdl, SMALL_IOS), Error::Damaged(_)));

    let mut bad_dlen = fresh.clone();
    let mut s = fr_sec;
    s[(f_slot + 1) * 32 + 8..(f_slot + 1) * 32 + 16].copy_from_slice(&512u64.to_le_bytes());
    s[(f_slot + 1) * 32 + 24..(f_slot + 1) * 32 + 32].copy_from_slice(&512u64.to_le_bytes());
    let new_sum = entry_set_checksum(&s[f_slot * 32..f_slot * 32 + set_len]);
    s[f_slot * 32 + 2..f_slot * 32 + 4].copy_from_slice(&new_sum.to_le_bytes());
    bad_dlen.set(fr_lba, &s);
    assert!(matches!(refused(&mut bad_dlen, SMALL_IOS), Error::Damaged(_)));

    let mut zero_with_clus = fresh.clone();
    let mut s = fr_sec;
    s[(f_slot + 1) * 32 + 8..(f_slot + 1) * 32 + 16].copy_from_slice(&0u64.to_le_bytes());
    s[(f_slot + 1) * 32 + 24..(f_slot + 1) * 32 + 32].copy_from_slice(&0u64.to_le_bytes());
    let new_sum = entry_set_checksum(&s[f_slot * 32..f_slot * 32 + set_len]);
    s[f_slot * 32 + 2..f_slot * 32 + 4].copy_from_slice(&new_sum.to_le_bytes());
    zero_with_clus.set(fr_lba, &s);
    assert!(matches!(refused(&mut zero_with_clus, SMALL_IOS), Error::Damaged(_)));

    let next_slot = f_slot + 1 + fr_sec[f_slot * 32 + 1] as usize;
    let mut cross = fresh.clone();
    let mut s = fr_sec;
    s[next_slot * 32..next_slot * 32 + set_len].copy_from_slice(&fr_sec[f_slot * 32..f_slot * 32 + set_len]);
    cross.set(fr_lba, &s);
    assert!(matches!(refused(&mut cross, SMALL_IOS), Error::Damaged(_)));

    let mut unknown_crit = fresh;
    let mut s = fr_sec;
    s[next_slot * 32] = 0x86;
    unknown_crit.set(fr_lba, &s);
    assert!(matches!(refused(&mut unknown_crit, SMALL_IOS), Error::Damaged(_)));
}

#[test]
fn system_files_past_cut_and_fragmented_bitmap_refused() {
    let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 8);
    let v = Exfat::read(&base, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let root_lba = v.lba(v.root);
    let bm_chain = v.chain(&base, v.bitmap_first);
    let up_chain = v.chain(&base, v.upcase_first);

    assert!(bm_chain.len() >= 2);
    let mut frag_bm = base.clone();
    let dst = 200u32;
    let data = frag_bm.read_bytes(v.lba(bm_chain[1]), v.cluster_sectors);
    frag_bm.set(v.lba(dst), &data);
    v.fat_set(&mut frag_bm, bm_chain[0], dst);
    v.fat_set(&mut frag_bm, dst, 0xffff_ffff);
    v.fat_set(&mut frag_bm, bm_chain[1], 0);
    v.bit_set(&mut frag_bm, bm_chain[1], false);
    v.bit_set(&mut frag_bm, dst, true);
    assert!(matches!(refused(&mut frag_bm, SMALL_IOS), Error::Volume(_)));

    let mut bm_past = base.clone();
    let new_bm = cut + 50;
    for (i, &c) in bm_chain.iter().enumerate() {
        v.bit_set(&mut bm_past, c, false);
        v.bit_set(&mut bm_past, new_bm + i as u32, true);
    }
    for (i, &c) in bm_chain.iter().enumerate() {
        let d = bm_past.read_bytes(v.lba(c), v.cluster_sectors);
        bm_past.set(v.lba(new_bm + i as u32), &d);
        v.fat_set(&mut bm_past, c, 0);
        let next = if i + 1 < bm_chain.len() {
            new_bm + i as u32 + 1
        } else {
            0xffff_ffff
        };
        v.fat_set(&mut bm_past, new_bm + i as u32, next);
    }
    let mut rsec = bm_past.sector(root_lba);
    let bm_slot = (0..16).find(|&i| rsec[i * 32] == 0x81).unwrap();
    rsec[bm_slot * 32 + 20..bm_slot * 32 + 24].copy_from_slice(&new_bm.to_le_bytes());
    bm_past.set(root_lba, &rsec);
    assert!(matches!(refused(&mut bm_past, SMALL_IOS), Error::Volume(_)));

    let mut up_past = base.clone();
    let new_up = cut + 100;
    for (i, &c) in up_chain.iter().enumerate() {
        let d = up_past.read_bytes(v.lba(c), v.cluster_sectors);
        up_past.set(v.lba(new_up + i as u32), &d);
        v.bit_set(&mut up_past, c, false);
        v.fat_set(&mut up_past, c, 0);
        v.bit_set(&mut up_past, new_up + i as u32, true);
        let next = if i + 1 < up_chain.len() {
            new_up + i as u32 + 1
        } else {
            0xffff_ffff
        };
        v.fat_set(&mut up_past, new_up + i as u32, next);
    }
    let mut rsec = up_past.sector(root_lba);
    let up_slot = (0..16).find(|&i| rsec[i * 32] == 0x82).unwrap();
    rsec[up_slot * 32 + 20..up_slot * 32 + 24].copy_from_slice(&new_up.to_le_bytes());
    up_past.set(root_lba, &rsec);
    assert!(matches!(refused(&mut up_past, SMALL_IOS), Error::Volume(_)));

    let mut root_past = base;
    let new_root = cut + 200;
    let rd = root_past.read_bytes(v.lba(v.root), v.cluster_sectors);
    root_past.set(v.lba(new_root), &rd);
    v.bit_set(&mut root_past, v.root, false);
    v.fat_set(&mut root_past, v.root, 0);
    v.bit_set(&mut root_past, new_root, true);
    v.fat_set(&mut root_past, new_root, 0xffff_ffff);
    let mut b = root_past.sector(FAT_FIRST);
    b[96..100].copy_from_slice(&new_root.to_le_bytes());
    root_past.set(FAT_FIRST, &b);
    recompute_boot_regions(&mut root_past);
    assert!(matches!(refused(&mut root_past, SMALL_IOS), Error::Volume(_)));
}
