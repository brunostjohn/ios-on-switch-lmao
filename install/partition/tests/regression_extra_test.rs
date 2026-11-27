// SPDX-License-Identifier: GPL-3.0-or-later
//! Regression tests for medium and low review findings (M1-M4, L1-L4).

use std::io::{Seek, SeekFrom, Write};
use std::process::Command;

use switch_card::{BASIC_DATA_TYPE, Card};
use switch_card_testing::{Event, Recorder, SparseImage, tables};
use switch_partition::Error;
use switch_partition::journal;
use switch_partition_testing::cards::{self, FAT_FIRST};
use switch_partition_testing::cuts::{self, first_difference};
use switch_partition_testing::fat32::Fat32;
use switch_partition_testing::fixtures::{self, SMALL_FAT, SMALL_IOS, SMALL_SECTORS, Shape};
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

#[test]
fn m1_bpb_fsver_extflags_and_cluster_cap_refused() {
    let base = fixtures::small(&SIMPLE);

    let mut fsver = base.clone();
    for lba in [FAT_FIRST, FAT_FIRST + 6] {
        let mut b = fsver.sector(lba);
        b[42..44].copy_from_slice(&1u16.to_le_bytes());
        fsver.set(lba, &b);
    }
    assert!(matches!(refused(&mut fsver, SMALL_IOS), Error::Volume(_)));

    let mut extflags = base;
    for lba in [FAT_FIRST, FAT_FIRST + 6] {
        let mut b = extflags.sector(lba);
        b[40..42].copy_from_slice(&0x0010u16.to_le_bytes());
        extflags.set(lba, &b);
    }
    assert!(matches!(refused(&mut extflags, SMALL_IOS), Error::Volume(_)));

    let sectors = 1u64 << 30;
    let fat_sectors = 0x1200_0000u64;
    let fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST, fat_sectors);
    let mut over_cap = tables::card(sectors, 128, 1, &[(0, fat)]);
    let mut b = [0u8; 512];
    b[11..13].copy_from_slice(&512u16.to_le_bytes());
    b[13] = 1;
    b[14..16].copy_from_slice(&32u16.to_le_bytes());
    b[16] = 2;
    b[32..36].copy_from_slice(&0x1100_0000u32.to_le_bytes());
    b[36..40].copy_from_slice(&0x0040_0000u32.to_le_bytes());
    b[44..48].copy_from_slice(&2u32.to_le_bytes());
    b[510] = 0x55;
    b[511] = 0xaa;
    over_cap.set(FAT_FIRST, &b);
    assert!(matches!(refused(&mut over_cap, SMALL_IOS), Error::Volume(_)));
}

#[test]
fn m2_lost_clusters_filesize_mismatch_and_cyclic_chain_refused() {
    let base = fixtures::small(&SIMPLE);
    let v = Fat32::read(&base, FAT_FIRST);

    let mut lost = base.clone();
    let free_c = v.free(&lost, 50, 1)[0];
    v.set(&mut lost, free_c, 0x0fff_ffff);
    assert!(matches!(refused(&mut lost, SMALL_IOS), Error::Damaged(_)));

    let mut cycle = base;
    let pair = v.free(&cycle, 100, 3);
    let (c0, c1, spare) = (pair[0], pair[1], pair[2]);
    let mut root = v.root_dir(&cycle, vec![spare]);
    v.file(&mut cycle, &mut root, "loop.bin", 1024, &[c0, c1], 1);
    v.set(&mut cycle, c1, c0);
    let err = refused(&mut cycle, SMALL_IOS);
    assert!(
        matches!(err, Error::Damaged(ref msg) if msg.contains("loops")),
        "got {err}"
    );

    let patch_entry = |image: &mut SparseImage, target_clus: u32, new_size: u32| {
        let root_lba = v.lba(v.root);
        let mut sec = image.sector(root_lba);
        for slot in 0..16 {
            let e = &mut sec[slot * 32..][..32];
            if e[0] != 0 && e[0] != 0xe5 && e[11] & 0x3f != 0x0f {
                let clus = (u32::from(u16::from_le_bytes([e[20], e[21]])) << 16)
                    | u32::from(u16::from_le_bytes([e[26], e[27]]));
                if clus == target_clus {
                    e[28..32].copy_from_slice(&new_size.to_le_bytes());
                    image.set(root_lba, &sec);
                    return;
                }
            }
        }
        panic!("entry for cluster {target_clus} not found");
    };

    let mut fs_base = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut fs_base, FAT_FIRST, SMALL_FAT, 1);
    let fv = Fat32::read(&fs_base, FAT_FIRST);
    let mut froot = fv.root_dir(&fs_base, vec![]);
    fv.file(&mut fs_base, &mut froot, "two.bin", 1024, &[200, 201], 7);
    fv.mkdir(&mut fs_base, &mut froot, "sub", vec![210]);
    fv.file(&mut fs_base, &mut froot, "zero.bin", 0, &[], 8);
    fv.finish(&mut fs_base);

    let mut nonzero_zero_clus = fs_base.clone();
    patch_entry(&mut nonzero_zero_clus, 0, 100);
    assert!(matches!(refused(&mut nonzero_zero_clus, SMALL_IOS), Error::Damaged(_)));

    let mut short_chain = fs_base.clone();
    patch_entry(&mut short_chain, 200, 2048);
    assert!(matches!(refused(&mut short_chain, SMALL_IOS), Error::Damaged(_)));

    let mut extra_chain = fs_base.clone();
    patch_entry(&mut extra_chain, 200, 512);
    assert!(matches!(refused(&mut extra_chain, SMALL_IOS), Error::Damaged(_)));

    let mut zero_with_clus = fs_base.clone();
    patch_entry(&mut zero_with_clus, 200, 0);
    assert!(matches!(refused(&mut zero_with_clus, SMALL_IOS), Error::Damaged(_)));

    let mut dir_nonzero = fs_base.clone();
    patch_entry(&mut dir_nonzero, 210, 512);
    assert!(matches!(refused(&mut dir_nonzero, SMALL_IOS), Error::Damaged(_)));

    scenario::partition(&mut fs_base, SMALL_IOS);
}

#[test]
fn m3_non_contiguous_mbr_slots_refused() {
    let sectors = 1u64 << 20;
    let mut image = SparseImage::new(sectors);
    tables::mbr_slot(&mut image, 0, 0x0c, FAT_FIRST, SMALL_FAT);
    tables::mbr_slot(&mut image, 2, 0x83, FAT_FIRST + SMALL_FAT, 1 << 18);
    fixtures::fill(&mut image, SMALL_FAT, 1, sectors - 33, SMALL_IOS, &SIMPLE);
    assert!(matches!(refused(&mut image, SMALL_IOS), Error::Mbr(_)));
}

#[test]
fn m4_fat_to_end_zeros_unused_backup_gpt_array() {
    let sectors = 1u64 << 20;
    let fat_sectors = sectors - FAT_FIRST;
    let mut image = fixtures::mbr_only_sized(sectors, fat_sectors, &[], &SIMPLE);
    for lba in (sectors - 32)..=(sectors - 2) {
        image.set(lba, &[0xde; 512]);
    }
    let before = reader::list(&mut image, FAT_FIRST, 0);
    let (plan, _) = scenario::partition(&mut image, SMALL_IOS);
    scenario::check_done(&mut image, &plan, &before, 0, "m4-fat-to-end");
    for lba in (sectors - 32)..=(sectors - 2) {
        assert_eq!(image.sector(lba), [0u8; 512], "backup GPT array LBA {lba} not zeroed");
    }
    macos::fsck_fat32(&image, FAT_FIRST, plan.fat_new_sectors, "m4-fat-to-end");
}

#[test]
fn l1_unaligned_ios_bytes_refused_without_truncation() {
    let mut image = fixtures::small(&SIMPLE);
    let survey = switch_partition::survey(&mut image).unwrap();
    let err = survey
        .plan(SMALL_IOS + 123, 0)
        .expect_err("must reject unaligned bytes");
    assert!(matches!(err, Error::Size { bytes, .. } if bytes == SMALL_IOS + 123));
}

#[test]
fn l2_tool_resume_exits_zero() {
    let mut image = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut image, FAT_FIRST, SMALL_FAT, 1);
    let plan = switch_partition::survey(&mut image)
        .unwrap()
        .plan(SMALL_IOS, 0)
        .unwrap();
    journal::begin(&mut image, &plan).unwrap();

    let path = macos::scratch("l2-tool.img");
    let mut f = std::fs::File::create(&path).unwrap();
    f.set_len(image.sectors * 512).unwrap();
    for (lba, s) in &image.data {
        f.seek(SeekFrom::Start(*lba * 512)).unwrap();
        f.write_all(&s[..]).unwrap();
    }
    drop(f);

    let tool = std::env::var("TOOL").unwrap();
    let out = Command::new(&tool).arg(&path).output().unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        out.status.success(),
        "tool must exit 0 after resume: status={:?} stdout={stdout} stderr={stderr}",
        out.status.code()
    );
    assert!(stdout.contains("resumed a cut-short run"));
    let file = std::fs::File::open(&path).unwrap();
    let mut dev = switch_card::StreamDevice::file(file, 512).unwrap();
    assert!(Card::open(&mut dev).is_ok());
    assert_eq!(switch_partition::pending(&mut dev).unwrap(), None);
    drop(dev);
    let _ = std::fs::remove_file(&path);
}

#[test]
fn l3_rootclus_writes_backup_before_main_with_barrier() {
    let mut image = fixtures::small(&Shape {
        root_in_cut: true,
        full_tail: false,
        fragments: 0,
        filler: false,
    });
    let events = {
        let mut rec = Recorder::new(&mut image);
        scenario::partition(&mut rec, SMALL_IOS);
        rec.events
    };
    let mut boot_writes = Vec::new();
    let mut epoch = 0usize;
    for ev in &events {
        match ev {
            Event::Barrier => epoch += 1,
            Event::Write(lba, data) => {
                let end = *lba + (data.len() / 512) as u64;
                if *lba <= FAT_FIRST && FAT_FIRST < end {
                    boot_writes.push((epoch, "main"));
                }
                if *lba <= FAT_FIRST + 6 && FAT_FIRST + 6 < end {
                    boot_writes.push((epoch, "backup"));
                }
            }
        }
    }
    assert!(boot_writes.len() >= 2);
    assert_eq!(boot_writes[0].1, "backup");
    assert_eq!(boot_writes[1].1, "main");
    assert!(boot_writes[0].0 < boot_writes[1].0);

    let base = fixtures::small(&Shape {
        root_in_cut: true,
        full_tail: false,
        fragments: 200,
        filler: false,
    });
    let mut done = base.clone();
    let events = {
        let mut rec = Recorder::new(&mut done);
        scenario::partition(&mut rec, SMALL_IOS);
        rec.events
    };
    let before = reader::list(&mut base.clone(), FAT_FIRST, 0);
    let mut epochs: Vec<Vec<(u64, Vec<u8>)>> = vec![Vec::new()];
    for ev in &events {
        match ev {
            Event::Write(lba, d) => epochs.last_mut().unwrap().push((*lba, d.clone())),
            Event::Barrier => epochs.push(Vec::new()),
        }
    }
    let mut prefix: Vec<(u64, Vec<u8>)> = Vec::new();
    for (ei, ep) in epochs.iter().enumerate() {
        for (wi, (lba, d)) in ep.iter().enumerate() {
            if d.len() > 512 {
                let mut landed = prefix.clone();
                landed.extend_from_slice(&ep[..wi]);
                let last_sec = d.len() / 512 - 1;
                landed.push((*lba + last_sec as u64, d[last_sec * 512..].to_vec()));
                let mut st = cuts::state(&base, &landed);
                for copy in [0, 1] {
                    assert_eq!(reader::list(&mut st, FAT_FIRST, copy), before);
                }
                scenario::recover(&mut st, SMALL_IOS);
                assert_eq!(
                    first_difference(&st, &done),
                    None,
                    "epoch {ei} write {wi} tail tear recovery mismatch"
                );
            }
        }
        if ep.len() >= 2 {
            let mut rev = ep.clone();
            rev.reverse();
            for k in 1..rev.len() {
                let mut landed = prefix.clone();
                landed.extend_from_slice(&rev[..k]);
                let mut st = cuts::state(&base, &landed);
                for copy in [0, 1] {
                    assert_eq!(reader::list(&mut st, FAT_FIRST, copy), before);
                }
                scenario::recover(&mut st, SMALL_IOS);
                assert_eq!(
                    first_difference(&st, &done),
                    None,
                    "epoch {ei} reversed prefix {k} recovery mismatch"
                );
            }
        }
        prefix.extend_from_slice(ep);
    }
}

#[test]
fn l4_minor_bounds_and_ffff_bpb_fields() {
    let tiny_fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", 64, 1024);
    let mut small_usable = tables::card(SMALL_SECTORS, 128, 1, &[(0, tiny_fat)]);
    for lba in [1u64, SMALL_SECTORS - 1] {
        let mut h = small_usable.sector(lba);
        h[48..56].copy_from_slice(&(journal::FIRST + 4).to_le_bytes());
        h[16..20].fill(0);
        let crc = crc32fast::hash(&h[..92]);
        h[16..20].copy_from_slice(&crc.to_le_bytes());
        small_usable.set(lba, &h);
    }
    assert!(matches!(refused(&mut small_usable, SMALL_IOS), Error::Journal(_)));

    let big_cap = 5_000_000_000u64;
    let big_fat_first = 65536u64;
    let big_fat_sectors = u64::from(u32::MAX);
    let big_fat = tables::entry(BASIC_DATA_TYPE, 1, "hos_data", big_fat_first, big_fat_sectors);
    let mut big = tables::card(big_cap, 128, 1, &[(0, big_fat)]);
    macos::format_fat32(&mut big, big_fat_first, 300_000, 1);
    let survey = switch_partition::survey(&mut big).unwrap();
    let limits = survey.limits(0);
    assert!(limits.possible());
    let plan = survey.plan(limits.max, limits.min).unwrap();
    assert!(plan.entry_last() <= u64::from(u32::MAX));

    let cap = 1u64 << 20;
    let l4t_start = FAT_FIRST + SMALL_FAT;
    let exact_len = (cap - 34) - l4t_start + 1;
    let mut ok_mbr = fixtures::mbr_only(cap, &[(0x83, l4t_start, exact_len)], &SIMPLE);
    assert!(switch_partition::survey(&mut ok_mbr).is_ok());
    let mut past_mbr = fixtures::mbr_only(cap, &[(0x83, l4t_start, exact_len + 1)], &SIMPLE);
    assert!(matches!(refused(&mut past_mbr, SMALL_IOS), Error::Mbr(_)));

    let mut ffff = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    macos::format_fat32(&mut ffff, FAT_FIRST, SMALL_FAT, 1);
    let mut b = ffff.sector(FAT_FIRST);
    b[48..50].copy_from_slice(&0xffffu16.to_le_bytes());
    b[50..52].copy_from_slice(&0xffffu16.to_le_bytes());
    ffff.set(FAT_FIRST, &b);
    let before = reader::list(&mut ffff, FAT_FIRST, 0);
    let (plan, _) = scenario::partition(&mut ffff, SMALL_IOS);
    scenario::check_done(&mut ffff, &plan, &before, 0, "l4-ffff");
}
