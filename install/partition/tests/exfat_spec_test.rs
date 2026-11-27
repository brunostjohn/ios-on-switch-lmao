// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT specification compliance tests: stale backup boot fields (H1), Volume GUID and TexFAT padding (M1),
//! tiny partitions (L1), vendor extensions, preallocated files, deep directories, and uncompressed up-case tables.

use switch_card::Card;
use switch_card_testing::SparseImage;
use switch_partition::Error;
use switch_partition::journal;
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::exfat::{Exfat, entry_set_checksum};
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

fn volume_guid_entry(guid: [u8; 16]) -> [u8; 32] {
    let mut e = [0u8; 32];
    e[0] = 0xa0;
    e[6..22].copy_from_slice(&guid);
    let sum = entry_set_checksum(&e);
    e[2..4].copy_from_slice(&sum.to_le_bytes());
    e
}

#[test]
fn h1_stale_backup_boot_flags_and_percent_in_use_accepted() {
    let mut img_pct = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut img_pct, FAT_FIRST, SMALL_FAT, 8);
    let mut main0 = img_pct.sector(FAT_FIRST);
    main0[112] = 42;
    img_pct.set(FAT_FIRST, &main0);
    let before = exfat_reader::list(&mut img_pct, FAT_FIRST, true);
    let (plan, _) = scenario::partition(&mut img_pct, SMALL_IOS);
    scenario::check_done(&mut img_pct, &plan, &before, 0, "h1-pct");
    macos::fsck_exfat(&img_pct, FAT_FIRST, plan.fat_new_sectors, "h1-pct");

    let mut img_bk_flag = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut img_bk_flag, FAT_FIRST, SMALL_FAT, 8);
    let mut bk0 = img_bk_flag.sector(FAT_FIRST + 12);
    bk0[106..108].copy_from_slice(&0x0002u16.to_le_bytes());
    img_bk_flag.set(FAT_FIRST + 12, &bk0);
    let before2 = exfat_reader::list(&mut img_bk_flag, FAT_FIRST, true);
    let (plan2, _) = scenario::partition(&mut img_bk_flag, SMALL_IOS);
    scenario::check_done(&mut img_bk_flag, &plan2, &before2, 0, "h1-bkflag");
    macos::fsck_exfat(&img_bk_flag, FAT_FIRST, plan2.fat_new_sectors, "h1-bkflag");

    let mut img_fin = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut img_fin, FAT_FIRST, SMALL_FAT, 8);
    let plan3 = switch_partition::survey(&mut img_fin)
        .unwrap()
        .plan(SMALL_IOS, 0)
        .unwrap();
    journal::begin(&mut img_fin, &plan3).unwrap();
    let mut main0 = img_fin.sector(FAT_FIRST);
    main0[112] = 15;
    img_fin.set(FAT_FIRST, &main0);
    let before3 = exfat_reader::list(&mut img_fin, FAT_FIRST, true);
    assert!(switch_partition::resume(&mut img_fin).unwrap().is_some());
    let fat_new3 = Card::open(&mut img_fin).unwrap().entries()[0].sectors();
    assert_eq!(exfat_reader::list(&mut img_fin, FAT_FIRST, true), before3);
    macos::fsck_exfat(&img_fin, FAT_FIRST, fat_new3, "h1-finished-resume");
}

#[test]
fn m1_volume_guid_validation_and_texfat_padding_refused() {
    let mut base = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    macos::format_exfat(&mut base, FAT_FIRST, SMALL_FAT, 1);
    let v = Exfat::read(&base, FAT_FIRST);
    let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
    let mut root = v.root_dir(&base, vec![]);
    let sub = v.mkdir(&mut base, &mut root, "sub", vec![500], true);
    let sub_lba = v.lba(sub.first);
    let root_lba = v.lba(v.root);

    let valid_guid = volume_guid_entry([
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01,
    ]);

    let mut sub_a0 = base.clone();
    let mut s = [0u8; 512];
    s[..32].copy_from_slice(&valid_guid);
    sub_a0.set(sub_lba, &s);
    assert!(matches!(switch_partition::survey(&mut sub_a0), Err(Error::Damaged(_))));

    let mut sub_a1 = base.clone();
    let mut s = [0u8; 512];
    s[0] = 0xa1;
    sub_a1.set(sub_lba, &s);
    assert!(matches!(switch_partition::survey(&mut sub_a1), Err(Error::Damaged(_))));

    let end_slot = {
        let rsec = base.sector(root_lba);
        (0..16).find(|&i| rsec[i * 32] == 0x00).unwrap()
    };

    let mut root_a1 = base.clone();
    let mut rsec = root_a1.sector(root_lba);
    rsec[end_slot * 32] = 0xa1;
    root_a1.set(root_lba, &rsec);
    assert!(matches!(switch_partition::survey(&mut root_a1), Err(Error::Damaged(_))));

    let mut bad_ck = base.clone();
    let mut rsec = bad_ck.sector(root_lba);
    let mut e = valid_guid;
    e[2] ^= 1;
    rsec[end_slot * 32..(end_slot + 1) * 32].copy_from_slice(&e);
    bad_ck.set(root_lba, &rsec);
    assert!(matches!(switch_partition::survey(&mut bad_ck), Err(Error::Damaged(_))));

    let mut bad_sec = base.clone();
    let mut rsec = bad_sec.sector(root_lba);
    let mut e = valid_guid;
    e[1] = 1;
    let sum = entry_set_checksum(&e);
    e[2..4].copy_from_slice(&sum.to_le_bytes());
    rsec[end_slot * 32..(end_slot + 1) * 32].copy_from_slice(&e);
    bad_sec.set(root_lba, &rsec);
    assert!(matches!(switch_partition::survey(&mut bad_sec), Err(Error::Damaged(_))));

    let mut dup_a0 = base.clone();
    let mut rsec = dup_a0.sector(root_lba);
    rsec[end_slot * 32..(end_slot + 1) * 32].copy_from_slice(&valid_guid);
    rsec[(end_slot + 1) * 32..(end_slot + 2) * 32].copy_from_slice(&valid_guid);
    dup_a0.set(root_lba, &rsec);
    assert!(matches!(switch_partition::survey(&mut dup_a0), Err(Error::Damaged(_))));

    let mut ok_a0 = base;
    let mut rsec = ok_a0.sector(root_lba);
    rsec[end_slot * 32..(end_slot + 1) * 32].copy_from_slice(&valid_guid);
    ok_a0.set(root_lba, &rsec);
    let mut r = v.root_dir(&ok_a0, vec![]);
    v.file(&mut ok_a0, &mut r, "tail.bin", 512, &[cut + 20], 77, true);
    let before = exfat_reader::list(&mut ok_a0, FAT_FIRST, true);
    macos::fsck_exfat(&ok_a0, FAT_FIRST, SMALL_FAT, "m1-valid-a0-pre");
    let (plan, _) = scenario::partition(&mut ok_a0, SMALL_IOS);
    scenario::check_done(&mut ok_a0, &plan, &before, 0, "m1-valid-a0");
    macos::fsck_exfat(&ok_a0, FAT_FIRST, plan.fat_new_sectors, "m1-valid-a0-post");
}

#[test]
fn l1_tiny_exfat_partition_returns_volume_error() {
    let mut b = [0u8; 512];
    b[..11].copy_from_slice(&[0xeb, 0x76, 0x90, b'E', b'X', b'F', b'A', b'T', b' ', b' ', b' ']);
    b[510] = 0x55;
    b[511] = 0xaa;

    let mut gpt_tiny = cards::gpt_exfat(FAT_FIRST + 16 + 34, 16);
    gpt_tiny.set(FAT_FIRST, &b);
    assert!(matches!(switch_partition::survey(&mut gpt_tiny), Err(Error::Volume(_))));

    let mut mbr_eof = cards::mbr_only_exfat(FAT_FIRST + 16, 16, &[]);
    mbr_eof.set(FAT_FIRST, &b);
    assert!(matches!(switch_partition::survey(&mut mbr_eof), Err(Error::Volume(_))));
}

#[test]
fn spec_entry_sets_limits_bitmap_and_upcase_edge_cases() {
    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&img, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&img, vec![]);

        v.file(
            &mut img,
            &mut root,
            "sparse.bin",
            2048,
            &[cut + 10, cut + 11, cut + 12, cut + 13],
            11,
            true,
        );
        let r_lba = v.lba(v.root);
        let mut rsec = img.sector(r_lba);
        let fslot = (0..16).find(|&i| rsec[i * 32] == 0x85).unwrap();
        let slen = (1 + rsec[fslot * 32 + 1] as usize) * 32;
        rsec[(fslot + 1) * 32 + 8..(fslot + 1) * 32 + 16].copy_from_slice(&512u64.to_le_bytes());
        let sum = entry_set_checksum(&rsec[fslot * 32..fslot * 32 + slen]);
        rsec[fslot * 32 + 2..fslot * 32 + 4].copy_from_slice(&sum.to_le_bytes());
        img.set(r_lba, &rsec);

        let mut nca_dir = v.mkdir(&mut img, &mut root, "split.nca", vec![cut + 20], true);
        v.file(&mut img, &mut nca_dir, "00", 512, &[cut + 21], 12, true);
        let mut rsec = img.sector(r_lba);
        let dslot = (fslot + 1 + rsec[fslot * 32 + 1] as usize..16)
            .find(|&i| rsec[i * 32] == 0x85)
            .unwrap();
        let dlen = (1 + rsec[dslot * 32 + 1] as usize) * 32;
        rsec[dslot * 32 + 4..dslot * 32 + 6].copy_from_slice(&0x0030u16.to_le_bytes());
        let sum = entry_set_checksum(&rsec[dslot * 32..dslot * 32 + dlen]);
        rsec[dslot * 32 + 2..dslot * 32 + 4].copy_from_slice(&sum.to_le_bytes());
        img.set(r_lba, &rsec);

        let mut rsec = img.sector(r_lba);
        let has_label = (0..16).any(|i| rsec[i * 32] == 0x83);
        let end_slot = (0..16).find(|&i| rsec[i * 32] == 0x00).unwrap();
        let eslot = if has_label {
            end_slot
        } else {
            rsec[end_slot * 32] = 0x83;
            rsec[end_slot * 32 + 1] = 0;
            end_slot + 1
        };
        let eclus = cut + 30;
        v.bit_set(&mut img, eclus, true);
        img.set(v.lba(eclus), &[0x77u8; 512]);
        let mut set = [0u8; 4 * 32];
        set[0] = 0x85;
        set[1] = 3;
        set[4..6].copy_from_slice(&0x0020u16.to_le_bytes());
        set[32] = 0xc0;
        set[33] = 0x03;
        set[35] = 5;
        let mut hash = 0u16;
        for u in "V.BIN".encode_utf16() {
            for b in u.to_le_bytes() {
                hash = hash.rotate_right(1).wrapping_add(u16::from(b));
            }
        }
        set[36..38].copy_from_slice(&hash.to_le_bytes());
        set[40..48].copy_from_slice(&512u64.to_le_bytes());
        set[52..56].copy_from_slice(&eclus.to_le_bytes());
        set[56..64].copy_from_slice(&512u64.to_le_bytes());
        set[64] = 0xc1;
        for (j, u) in "v.bin".encode_utf16().enumerate() {
            set[66 + j * 2..68 + j * 2].copy_from_slice(&u.to_le_bytes());
        }
        set[96] = 0xe0;
        set[98..114].copy_from_slice(&[0xab; 16]);
        let sum = entry_set_checksum(&set);
        set[2..4].copy_from_slice(&sum.to_le_bytes());
        rsec[eslot * 32..(eslot + 4) * 32].copy_from_slice(&set);
        img.set(r_lba, &rsec);

        let before = exfat_reader::list(&mut img, FAT_FIRST, true);
        let (plan, _) = scenario::partition(&mut img, SMALL_IOS);
        scenario::check_done(&mut img, &plan, &before, 0, "exp06b");
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp06b");
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let mut b = img.sector(FAT_FIRST);
        b[64..72].fill(0);
        img.set(FAT_FIRST, &b);
        recompute_boot_regions(&mut img);
        let v = Exfat::read(&img, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut root = v.root_dir(&img, vec![]);
        let pieces: Vec<u32> = (0..1000u32)
            .map(|i| if i % 2 == 0 { cut + 100 + 2 * i } else { 1000 + 2 * i })
            .collect();
        v.file(
            &mut img,
            &mut root,
            "frag1000.bin",
            pieces.len() * 512,
            &pieces,
            707,
            false,
        );

        let survey = switch_partition::survey(&mut img).unwrap();
        let limits = survey.limits(0);
        let plan_max = survey.plan(limits.max, 0).unwrap();
        assert!(survey.plan(limits.max + (16 << 20), 0).is_err());
        let before = exfat_reader::list(&mut img, FAT_FIRST, true);
        switch_partition::run(&mut img, &plan_max).unwrap();
        scenario::check_done(&mut img, &plan_max, &before, 0, "exp06e");
        macos::fsck_exfat(&img, FAT_FIRST, plan_max.fat_new_sectors, "exp06e");
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 8);
        let v0 = Exfat::read(&img, FAT_FIRST);
        assert_eq!(v0.chain(&img, v0.bitmap_first).len(), 2);
        let (plan, _) = scenario::partition(&mut img, 128 << 20);
        let v1 = Exfat::read(&img, FAT_FIRST);
        assert_eq!(v1.chain(&img, v1.bitmap_first).len(), 1);
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp08a");

        let mut img_tail = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img_tail, FAT_FIRST, SMALL_FAT - 3, 1);
        let vt = Exfat::read(&img_tail, FAT_FIRST);
        let rem = vt.clusters % 8;
        assert_ne!(rem, 0);
        let last_byte = (vt.clusters as u64 - 1) / 8;
        let cb = vt.cluster_bytes() as u64;
        let bm_c = vt.bitmap_first + (last_byte / cb) as u32;
        let within = last_byte % cb;
        let lba = vt.lba(bm_c) + within / 512;
        let mut s = img_tail.sector(lba);
        s[(within % 512) as usize] |= 0xffu8 << rem;
        img_tail.set(lba, &s);
        let (plan_t, _) = scenario::partition(&mut img_tail, SMALL_IOS);
        macos::fsck_exfat(&img_tail, FAT_FIRST, plan_t.fat_new_sectors, "exp08b");
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&img, FAT_FIRST);
        let cut = exfat_fixtures::cut_cluster(&v, SMALL_FAT, SMALL_SECTORS - 33, SMALL_IOS);
        let mut dir = v.root_dir(&img, vec![]);
        for depth in 0..100u32 {
            dir = v.mkdir(
                &mut img,
                &mut dir,
                &format!("d{depth}"),
                vec![cut + 1000 + depth],
                depth % 2 == 0,
            );
        }
        v.file(
            &mut img,
            &mut dir,
            "leaf.bin",
            1024,
            &[cut + 2000, cut + 2001],
            42,
            true,
        );
        let d_lba = v.lba(dir.first);
        let mut dsec = img.sector(d_lba);
        let slen = (1 + dsec[1] as usize) * 32;
        dsec[32 + 8..32 + 16].fill(0);
        let sum = entry_set_checksum(&dsec[..slen]);
        dsec[2..4].copy_from_slice(&sum.to_le_bytes());
        img.set(d_lba, &dsec);

        let before = exfat_reader::list(&mut img, FAT_FIRST, true);
        let (plan, _) = scenario::partition(&mut img, SMALL_IOS);
        scenario::check_done(&mut img, &plan, &before, 0, "exp08c");
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp08c");
    }

    {
        let mut img = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
        macos::format_exfat(&mut img, FAT_FIRST, SMALL_FAT, 1);
        let v = Exfat::read(&img, FAT_FIRST);
        let mut full_up = Vec::with_capacity(131_072);
        for cp in 0..=u16::MAX {
            let up = if (b'a' as u16..=b'z' as u16).contains(&cp) {
                cp - 32
            } else {
                cp
            };
            full_up.extend_from_slice(&up.to_le_bytes());
        }
        let mut up_sum = 0u32;
        for &b in &full_up {
            up_sum = up_sum.rotate_right(1).wrapping_add(u32::from(b));
        }
        for c in v.chain(&img, v.upcase_first) {
            v.bit_set(&mut img, c, false);
            v.fat_set(&mut img, c, 0);
        }
        let new_up = 2000u32;
        for i in 0..256u32 {
            let c = new_up + i;
            v.bit_set(&mut img, c, true);
            v.fat_set(&mut img, c, if i + 1 < 256 { c + 1 } else { 0xffff_ffff });
            img.set(v.lba(c), &full_up[i as usize * 512..(i as usize + 1) * 512]);
        }
        let r_lba = v.lba(v.root);
        let mut rsec = img.sector(r_lba);
        let up_slot = (0..16).find(|&i| rsec[i * 32] == 0x82).unwrap();
        rsec[up_slot * 32 + 4..up_slot * 32 + 8].copy_from_slice(&up_sum.to_le_bytes());
        rsec[up_slot * 32 + 20..up_slot * 32 + 24].copy_from_slice(&new_up.to_le_bytes());
        rsec[up_slot * 32 + 24..up_slot * 32 + 32].copy_from_slice(&131_072u64.to_le_bytes());
        img.set(r_lba, &rsec);

        let (plan, _) = scenario::partition(&mut img, SMALL_IOS);
        macos::fsck_exfat(&img, FAT_FIRST, plan.fat_new_sectors, "exp08d");
    }
}
