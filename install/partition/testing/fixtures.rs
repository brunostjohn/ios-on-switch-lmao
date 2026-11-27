// SPDX-License-Identifier: GPL-3.0-or-later
//! The volumes the tests shrink: files and directories placed across the cut on purpose.

use switch_card_testing::SparseImage;

use crate::cards::{self, FAT_FIRST};
use crate::fat32::{Dir, Fat32, run};
use crate::macos;

/// A 256 MiB GPT card, its FAT32 from LBA 32768 to 1 MiB before the end, in 512-byte clusters.
pub const SMALL_SECTORS: u64 = 1 << 19;
pub const SMALL_FAT: u64 = SMALL_SECTORS - FAT_FIRST - 2048;
pub const SMALL_IOS: u64 = 64 << 20;

/// The first cluster a run carving `ios_bytes` cuts away (the same arithmetic as the crate's, done again).
pub fn cut_cluster(v: &Fat32, fat_sectors: u64, entry_limit: u64, ios_bytes: u64) -> u32 {
    let end = (FAT_FIRST + fat_sectors).min(entry_limit) / 0x8000 * 0x8000;
    let fat_new = end - ios_bytes / 512 - FAT_FIRST;
    ((fat_new - v.data_start) / v.cluster_sectors) as u32 + 2
}

pub struct Shape {
    /// The root directory's first cluster moves into the cut before anything else goes on.
    pub root_in_cut: bool,
    /// Every cluster past the cut that nothing else takes holds a filler file.
    pub full_tail: bool,
    /// One more file in this many one-cluster pieces, every other one past the cut: a run each, so several batches.
    pub fragments: u32,
    /// Zero-filled files from cluster 11,000 up to the cut, with holes, so moved runs split across free pieces.
    pub filler: bool,
}

/// A volume `fat_sectors` long at LBA 32768 of `image`, formatted with `cluster` sectors a cluster and filled as
/// `shape` says, with the cut where a run carving `ios_bytes` puts it (`entry_limit`: one past the last LBA the new
/// entry may use).
pub fn fill(image: &mut SparseImage, fat_sectors: u64, cluster: u32, entry_limit: u64, ios_bytes: u64, shape: &Shape) {
    macos::format_fat32(image, FAT_FIRST, fat_sectors, cluster);
    let v = Fat32::read(image, FAT_FIRST);
    let cut = cut_cluster(&v, fat_sectors, entry_limit, ios_bytes);
    let end = v.clusters + 2;
    assert!(cut + 12_000 < end, "the fixture needs 12,000 clusters past the cut");
    assert!(cut > 12_000, "and 12,000 below it");
    if shape.root_in_cut {
        move_root(image, &v, cut + 10_050);
    }
    let v = Fat32::read(image, FAT_FIRST);
    let mut root = v.root_dir(image, vec![cut + 10_100, 700, cut + 10_101]);
    let mut seed = 1;
    let mut file = |image: &mut SparseImage, dir: &mut Dir, name: &str, len: usize, clusters: &[u32]| {
        seed += 1;
        v.file(image, dir, name, len, clusters, seed);
    };
    let cb = v.cluster_bytes();
    file(
        image,
        &mut root,
        "keep below the cut.bin",
        100 << 10,
        &run(1000, (100usize << 10).div_ceil(cb)),
    );
    let mut nintendo = v.mkdir(image, &mut root, "Nintendo", vec![cut + 10_200, cut + 10_210, 1500]);
    let mut contents = v.mkdir(image, &mut nintendo, "Contents", vec![cut + 10_300, cut + 10_301]);
    let mut registered = v.mkdir(image, &mut contents, "registered", vec![1600, cut + 10_310]);
    file(
        image,
        &mut registered,
        "00000000000000000000000000000001.nca",
        6 * cb - 100,
        &[cut + 10_320, cut + 10_321, 1601, cut + 10_322, cut + 10_330, 1602],
    );
    let straddle = (600usize << 10).div_ceil(cb);
    file(
        image,
        &mut contents,
        "straddles the cut.bin",
        600 << 10,
        &run(cut - straddle as u32 / 2, straddle),
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
    );
    file(
        image,
        &mut contents,
        "big.bin",
        2 << 20,
        &run(cut + 2000, (2usize << 20).div_ceil(cb)),
    );
    let spare: Vec<u32> = (0..30u32).flat_map(|i| [3000 + i, cut + 7000 + 10 * i]).collect();
    let mut long = v.mkdir(image, &mut root, "a directory with a long name and many entries", spare);
    for i in 0..40u32 {
        let len = 1000 + 37 * i as usize;
        let clusters = run(cut + 8000 + 10 * i, len.div_ceil(cb));
        file(
            image,
            &mut long,
            &format!("saved game {i:02} with a name long enough for three entries.dat"),
            len,
            &clusters,
        );
    }
    let mut a = v.mkdir(image, &mut root, "a", vec![cut + 9000]);
    let mut b = v.mkdir(image, &mut a, "b", vec![6000]);
    let mut c = v.mkdir(image, &mut b, "c", vec![cut + 9100, cut + 9101]);
    file(
        image,
        &mut c,
        "deep.bin",
        5000,
        &run(cut + 9200, 5000usize.div_ceil(cb)),
    );
    file(image, &mut root, "empty.txt", 0, &[]);
    if shape.fragments > 0 {
        let pieces: Vec<u32> = (0..shape.fragments)
            .map(|i| if i % 2 == 0 { cut + 10_400 + 2 * i } else { 5000 + 2 * i })
            .collect();
        file(image, &mut root, "in many pieces.bin", pieces.len() * cb, &pieces);
    }
    let holes = if shape.full_tail { 150 } else { 50 };
    let filler_clusters = ((cut as usize / 200) * 2 + 2).div_ceil(cb / 32) + 1;
    assert!(
        filler_clusters <= 4000,
        "the filler directory's spares fit between 7,000 and 11,000"
    );
    let mut filler = v.mkdir(image, &mut root, "filler", run(7000, filler_clusters));
    let mut at = 11_000;
    let mut n = 0;
    while shape.filler && at + 200 < cut - 1000 {
        let used = run(at + holes, 200 - holes as usize);
        v.file_zero(image, &mut filler, &format!("below {n}"), used.len() * cb, &used);
        at += 200;
        n += 1;
    }
    if shape.full_tail {
        let mut tail = v.mkdir(image, &mut root, "tail", run(4500, 400));
        let mut n = 0;
        let mut c = cut;
        while c < end {
            let free: Vec<u32> = (c..end.min(c + 2000)).filter(|&x| v.get(image, x) == 0).collect();
            if !free.is_empty() {
                v.file_zero(image, &mut tail, &format!("tail {n}"), free.len() * cb, &free);
                n += 1;
            }
            c += 2000;
        }
    }
    v.finish(image);
}

/// The root directory's first cluster copied to `to`, and the boot sector and its backup pointed there.
fn move_root(image: &mut SparseImage, v: &Fat32, to: u32) {
    let data = image.read_bytes(v.lba(v.root), v.cluster_sectors);
    image.set(v.lba(to), &data);
    v.set(image, to, v.get(image, v.root));
    v.set(image, v.root, 0);
    for lba in [FAT_FIRST, FAT_FIRST + 6] {
        let mut s = image.sector(lba);
        s[44..48].copy_from_slice(&to.to_le_bytes());
        image.set(lba, &s);
    }
}

pub fn small(shape: &Shape) -> SparseImage {
    let mut image = cards::gpt(SMALL_SECTORS, SMALL_FAT);
    fill(&mut image, SMALL_FAT, 1, SMALL_SECTORS - 33, SMALL_IOS, shape);
    image
}

/// hekate's MBR-only card of `sectors`: the FAT (as `small`'s) in slot 0, then `rest` (type, first, sectors).
pub fn mbr_only(sectors: u64, rest: &[(u8, u64, u64)], shape: &Shape) -> SparseImage {
    mbr_only_sized(sectors, SMALL_FAT, rest, shape)
}

pub fn mbr_only_sized(sectors: u64, fat_sectors: u64, rest: &[(u8, u64, u64)], shape: &Shape) -> SparseImage {
    let mut image = cards::mbr_only(sectors, fat_sectors, rest);
    fill(&mut image, fat_sectors, 1, sectors - 33, SMALL_IOS, shape);
    image
}
