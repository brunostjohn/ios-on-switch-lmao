// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT volumes the tests shrink: `NoFatChain` and FAT-chained files and subdirectories, sector- and
//! cluster-straddling entry sets, and root tail clusters placed across the cut on purpose.

use switch_card_testing::SparseImage;

use crate::cards::{self, FAT_FIRST};
use crate::exfat::{Exfat, ExfatDir};
use crate::fat32::run;
use crate::fixtures::{SMALL_FAT, SMALL_IOS, SMALL_SECTORS};
use crate::macos;

pub fn cut_cluster(v: &Exfat, fat_sectors: u64, entry_limit: u64, ios_bytes: u64) -> u32 {
    let end = (FAT_FIRST + fat_sectors).min(entry_limit) / 0x8000 * 0x8000;
    let fat_new = end - ios_bytes / 512 - FAT_FIRST;
    ((fat_new - v.heap_offset) / v.cluster_sectors) as u32 + 2
}

pub struct ExfatShape {
    pub full_tail: bool,
    pub fragments: u32,
    pub filler: bool,
}

pub fn fill_exfat(
    image: &mut SparseImage,
    fat_sectors: u64,
    cluster: u32,
    entry_limit: u64,
    ios_bytes: u64,
    shape: &ExfatShape,
) {
    macos::format_exfat(image, FAT_FIRST, fat_sectors, cluster);
    let v = Exfat::read(image, FAT_FIRST);
    let cut = cut_cluster(&v, fat_sectors, entry_limit, ios_bytes);
    let end = v.clusters + 2;
    assert!(cut + 12_000 < end, "the fixture needs 12,000 clusters past the cut");
    assert!(cut > 12_000, "and 12,000 below it");

    let mut root = v.root_dir(image, vec![cut + 10_100, 700, cut + 10_101, 701, cut + 10_102]);
    let mut seed = 100u64;
    let mut file =
        |image: &mut SparseImage, dir: &mut ExfatDir, name: &str, len: usize, clusters: &[u32], nfc: bool| {
            seed += 1;
            v.file(image, dir, name, len, clusters, seed, nfc);
        };
    let cb = v.cluster_bytes();

    file(
        image,
        &mut root,
        "keep below the cut.bin",
        100 << 10,
        &run(1000, (100usize << 10).div_ceil(cb)),
        true,
    );

    let mut below_nfc_dir = v.mkdir(image, &mut root, "below-nfc", vec![1400, 1401], true);
    file(
        image,
        &mut below_nfc_dir,
        "child-in-cut.bin",
        3 * cb - 17,
        &run(cut + 10_150, 3),
        true,
    );

    // Slot 15 (byte 480) splits 0x85 and 0xC0 across a 512-byte sector boundary (and cluster boundary at 512B).
    let mut pad_n = 0u32;
    while v.slot_in_sector(&root) != 15 {
        let name = if (15 + 16 - v.slot_in_sector(&root)) % 16 == 4 {
            format!("pad-long-name-entry-{pad_n:02}.txt")
        } else {
            format!("pad{pad_n:02}.txt")
        };
        file(image, &mut root, &name, 0, &[], true);
        pad_n += 1;
    }
    assert_eq!(v.slot_in_sector(&root), 15);
    file(
        image,
        &mut root,
        "straddles-entry-sector.bin",
        4 * cb - 5,
        &run(cut + 10_160, 4),
        true,
    );

    let mut nintendo = v.mkdir(
        image,
        &mut root,
        "Nintendo",
        vec![cut + 10_200, cut + 10_210, 1500],
        false,
    );
    let mut contents = v.mkdir(image, &mut nintendo, "Contents", vec![cut + 10_300, cut + 10_301], true);
    let mut registered = v.mkdir(image, &mut contents, "registered", vec![1600, cut + 10_310], false);
    file(
        image,
        &mut registered,
        "00000000000000000000000000000001.nca",
        6 * cb - 100,
        &[cut + 10_320, cut + 10_321, 1601, cut + 10_322, cut + 10_330, 1602],
        false,
    );

    let straddle = (600usize << 10).div_ceil(cb);
    file(
        image,
        &mut contents,
        "straddles the cut.bin",
        600 << 10,
        &run(cut - straddle as u32 / 2, straddle),
        true,
    );

    let frag: Vec<u32> = (0..6u32)
        .flat_map(|i| {
            run(
                if i % 2 == 0 {
                    cut + 1000 + 200 * i
                } else {
                    2000 + 100 * i
                },
                100,
            )
        })
        .collect();
    file(
        image,
        &mut contents,
        "fragmented in and out.bin",
        frag.len() * cb,
        &frag,
        false,
    );

    file(
        image,
        &mut contents,
        "big-nfc.bin",
        2 << 20,
        &run(cut + 2000, (2usize << 20).div_ceil(cb)),
        true,
    );

    let long_clusters: Vec<u32> = (0..12u32).flat_map(|i| [3000 + i, cut + 7000 + 10 * i]).collect();
    let mut long = v.mkdir(
        image,
        &mut root,
        "a directory with a long name and many entries",
        long_clusters,
        false,
    );
    for i in 0..40u32 {
        let len = 1000 + 37 * i as usize;
        let clusters = run(cut + 8000 + 10 * i, len.div_ceil(cb));
        file(
            image,
            &mut long,
            &format!("saved game {i:02} with a name long enough for multiple entries.dat"),
            len,
            &clusters,
            i % 2 == 0,
        );
    }

    let mut a = v.mkdir(image, &mut root, "a", vec![cut + 9000], true);
    let mut b = v.mkdir(image, &mut a, "b", vec![6000], false);
    let mut c = v.mkdir(image, &mut b, "c", vec![cut + 9100, cut + 9101], true);
    file(
        image,
        &mut c,
        "deep.bin",
        5000,
        &run(cut + 9200, 5000usize.div_ceil(cb)),
        true,
    );
    file(image, &mut root, "empty.txt", 0, &[], true);

    if shape.fragments > 0 {
        let pieces: Vec<u32> = (0..shape.fragments)
            .map(|i| if i % 2 == 0 { cut + 10_400 + 2 * i } else { 5000 + 2 * i })
            .collect();
        file(
            image,
            &mut root,
            "in many pieces.bin",
            pieces.len() * cb,
            &pieces,
            false,
        );
    }

    let holes = if shape.full_tail { 150 } else { 50 };
    if shape.filler {
        let count = ((cut - 1000).saturating_sub(11_000) / 200) as usize;
        let filler_clusters = (count * 3 + 2).div_ceil(cb / 32).max(1);
        let mut filler = v.mkdir(image, &mut root, "filler", run(7000, filler_clusters), false);
        let mut at = 11_000;
        let mut n = 0;
        while at + 200 < cut - 1000 {
            let used = run(at + holes, 200 - holes as usize);
            v.file_zero(image, &mut filler, &format!("below {n}"), used.len() * cb, &used, true);
            at += 200;
            n += 1;
        }
    }

    if shape.full_tail {
        let mut tail = v.mkdir(image, &mut root, "tail", run(4500, 300), false);
        let mut n = 0;
        let mut cur = cut;
        while cur < end {
            let free: Vec<u32> = (cur..end.min(cur + 2000)).filter(|&x| !v.bit_get(image, x)).collect();
            if !free.is_empty() {
                let contiguous = free.windows(2).all(|w| w[1] == w[0] + 1);
                v.file_zero(
                    image,
                    &mut tail,
                    &format!("tail {n}"),
                    free.len() * cb,
                    &free,
                    contiguous,
                );
                n += 1;
            }
            cur += 2000;
        }
    }
}

pub fn small_exfat(shape: &ExfatShape) -> SparseImage {
    let mut image = cards::gpt_exfat(SMALL_SECTORS, SMALL_FAT);
    fill_exfat(&mut image, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, shape);
    image
}

pub fn mbr_only_exfat(sectors: u64, rest: &[(u8, u64, u64)], shape: &ExfatShape) -> SparseImage {
    mbr_only_exfat_sized(sectors, SMALL_FAT, rest, shape)
}

pub fn mbr_only_exfat_sized(
    sectors: u64,
    fat_sectors: u64,
    rest: &[(u8, u64, u64)],
    shape: &ExfatShape,
) -> SparseImage {
    let mut image = cards::mbr_only_exfat(sectors, fat_sectors, rest);
    fill_exfat(&mut image, fat_sectors, 1, sectors - 33, SMALL_IOS, shape);
    image
}
