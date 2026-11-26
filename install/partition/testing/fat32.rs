// SPDX-License-Identifier: GPL-3.0-or-later
//! Files and directories put on a FAT32 volume at exactly the clusters a test names, with long names, both FATs
//! kept equal, and contents a seed regenerates.

use std::collections::VecDeque;

use switch_card_testing::SparseImage;

const EOC: u32 = 0x0fff_ffff;

#[derive(Clone, Copy, Debug)]
pub struct Fat32 {
    pub first: u64,
    pub cluster_sectors: u64,
    pub reserved: u64,
    pub fats: u64,
    pub fat_sectors: u64,
    pub data_start: u64,
    pub clusters: u32,
    pub root: u32,
}

/// A directory being filled: its chain, the next free slot, and the clusters it grows into when full.
pub struct Dir {
    pub first: u32,
    chain: Vec<u32>,
    slot: usize,
    root: bool,
    pub spare: VecDeque<u32>,
}

/// Bytes a seed stands for: splitmix64, eight at a time.
pub fn content(seed: u64, len: usize) -> Vec<u8> {
    let mut state = seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) ^ 0x5157_4954_4348;
    let mut out = Vec::with_capacity(len + 8);
    while out.len() < len {
        state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = state;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        out.extend_from_slice(&(z ^ (z >> 31)).to_le_bytes());
    }
    out.truncate(len);
    out
}

impl Fat32 {
    pub fn read(image: &SparseImage, first: u64) -> Fat32 {
        let b = image.sector(first);
        let u16_at = |at: usize| u64::from(u16::from_le_bytes([b[at], b[at + 1]]));
        let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
        let reserved = u16_at(14);
        let fats = u64::from(b[16]);
        let fat_sectors = u64::from(u32_at(36));
        let cluster_sectors = u64::from(b[13]);
        let data_start = reserved + fats * fat_sectors;
        Fat32 {
            first,
            cluster_sectors,
            reserved,
            fats,
            fat_sectors,
            data_start,
            clusters: ((u64::from(u32_at(32)) - data_start) / cluster_sectors) as u32,
            root: u32_at(44),
        }
    }

    pub fn cluster_bytes(&self) -> usize {
        self.cluster_sectors as usize * 512
    }

    pub fn lba(&self, cluster: u32) -> u64 {
        self.first + self.data_start + u64::from(cluster - 2) * self.cluster_sectors
    }

    fn fat_at(&self, copy: u64, cluster: u32) -> (u64, usize) {
        let byte = u64::from(cluster) * 4;
        (
            self.first + self.reserved + copy * self.fat_sectors + byte / 512,
            (byte % 512) as usize,
        )
    }

    pub fn get(&self, image: &SparseImage, cluster: u32) -> u32 {
        let (lba, at) = self.fat_at(0, cluster);
        u32::from_le_bytes(image.sector(lba)[at..at + 4].try_into().unwrap()) & 0x0fff_ffff
    }

    pub fn set(&self, image: &mut SparseImage, cluster: u32, value: u32) {
        for copy in 0..self.fats {
            let (lba, at) = self.fat_at(copy, cluster);
            let mut s = image.sector(lba);
            s[at..at + 4].copy_from_slice(&value.to_le_bytes());
            image.set(lba, &s);
        }
    }

    /// The first `n` free clusters at or after `from`.
    pub fn free(&self, image: &SparseImage, from: u32, n: usize) -> Vec<u32> {
        (from..self.clusters + 2)
            .filter(|&c| self.get(image, c) == 0)
            .take(n)
            .collect()
    }

    fn link(&self, image: &mut SparseImage, chain: &[u32]) {
        for (i, &c) in chain.iter().enumerate() {
            assert_eq!(self.get(image, c), 0, "cluster {c} is taken");
            self.set(image, c, chain.get(i + 1).copied().unwrap_or(EOC));
        }
    }

    pub fn root_dir(&self, image: &SparseImage, spare: Vec<u32>) -> Dir {
        let mut slot = 0;
        let per = self.cluster_bytes() / 32;
        let s = image.read_bytes(self.lba(self.root), self.cluster_sectors);
        while slot < per && s[slot * 32] != 0 {
            slot += 1;
        }
        Dir {
            first: self.root,
            chain: vec![self.root],
            slot,
            root: true,
            spare: spare.into(),
        }
    }

    fn entry_slot(&self, image: &mut SparseImage, dir: &mut Dir) -> (u64, usize) {
        let per = self.cluster_bytes() / 32;
        if dir.slot == dir.chain.len() * per {
            let next = dir
                .spare
                .pop_front()
                .expect("the directory needs a spare cluster to grow into");
            assert_eq!(self.get(image, next), 0, "cluster {next} is taken");
            self.set(image, *dir.chain.last().unwrap(), next);
            self.set(image, next, EOC);
            image.set(self.lba(next), &vec![0; self.cluster_bytes()]);
            dir.chain.push(next);
        }
        let cluster = dir.chain[dir.slot / per];
        let within = (dir.slot % per) * 32;
        dir.slot += 1;
        (self.lba(cluster) + within as u64 / 512, within % 512)
    }

    fn put_entry(&self, image: &mut SparseImage, dir: &mut Dir, entry: &[u8; 32]) {
        let (lba, at) = self.entry_slot(image, dir);
        let mut s = image.sector(lba);
        s[at..at + 32].copy_from_slice(entry);
        image.set(lba, &s);
    }

    /// The long name's entries, then a short one `short` (11 bytes) for `first` cluster, `size` bytes.
    fn add(
        &self,
        image: &mut SparseImage,
        dir: &mut Dir,
        name: &str,
        short: [u8; 11],
        attr: u8,
        first: u32,
        size: u32,
    ) {
        let units: Vec<u16> = name.encode_utf16().collect();
        let n = units.len().div_ceil(13);
        let sum = short
            .iter()
            .fold(0u8, |s, &b| ((s & 1) << 7).wrapping_add(s >> 1).wrapping_add(b));
        for k in (1..=n).rev() {
            let mut e = [0u8; 32];
            e[0] = k as u8 | if k == n { 0x40 } else { 0 };
            e[11] = 0x0f;
            e[13] = sum;
            let at = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
            for (j, &pos) in at.iter().enumerate() {
                let i = (k - 1) * 13 + j;
                let u = match i.cmp(&units.len()) {
                    std::cmp::Ordering::Less => units[i],
                    std::cmp::Ordering::Equal => 0,
                    std::cmp::Ordering::Greater => 0xffff,
                };
                e[pos..pos + 2].copy_from_slice(&u.to_le_bytes());
            }
            self.put_entry(image, dir, &e);
        }
        let mut e = [0u8; 32];
        e[..11].copy_from_slice(&short);
        e[11] = attr;
        let date = (((2026 - 1980) << 9 | 11 << 5 | 26) as u16).to_le_bytes();
        for at in [16, 18, 24] {
            e[at..at + 2].copy_from_slice(&date);
        }
        e[20..22].copy_from_slice(&((first >> 16) as u16).to_le_bytes());
        e[26..28].copy_from_slice(&(first as u16).to_le_bytes());
        e[28..32].copy_from_slice(&size.to_le_bytes());
        self.put_entry(image, dir, &e);
    }

    /// A subdirectory over `clusters`, its first ones in use and the rest spare for growing.
    pub fn mkdir(&self, image: &mut SparseImage, parent: &mut Dir, name: &str, mut clusters: Vec<u32>) -> Dir {
        let first = clusters.remove(0);
        self.link(image, &[first]);
        image.set(self.lba(first), &vec![0; self.cluster_bytes()]);
        self.add(image, parent, name, short_name(), 0x10, first, 0);
        let mut dir = Dir {
            first,
            chain: vec![first],
            slot: 0,
            root: false,
            spare: clusters.into(),
        };
        let up = if parent.root { 0 } else { parent.first };
        for (dots, at) in [(b".          ", first), (b"..         ", up)] {
            let mut e = [0u8; 32];
            e[..11].copy_from_slice(dots);
            e[11] = 0x10;
            e[20..22].copy_from_slice(&((at >> 16) as u16).to_le_bytes());
            e[26..28].copy_from_slice(&(at as u16).to_le_bytes());
            self.put_entry(image, &mut dir, &e);
        }
        dir
    }

    /// A file of `len` bytes from `seed` in `clusters` (as many as it needs, in chain order).
    pub fn file(&self, image: &mut SparseImage, parent: &mut Dir, name: &str, len: usize, clusters: &[u32], seed: u64) {
        let need = len.div_ceil(self.cluster_bytes());
        assert_eq!(clusters.len(), need, "{name}: {need} clusters");
        self.link(image, clusters);
        let data = content(seed, len);
        let cb = self.cluster_bytes();
        for (i, &c) in clusters.iter().enumerate() {
            let mut chunk = data[i * cb..((i + 1) * cb).min(len)].to_vec();
            chunk.resize(cb, 0);
            image.set(self.lba(c), &chunk);
        }
        self.add(
            image,
            parent,
            name,
            short_name(),
            0x20,
            clusters.first().copied().unwrap_or(0),
            len as u32,
        );
    }

    /// A file of `len` zero bytes in `clusters`: its chain and entry, its data never written.
    pub fn file_zero(&self, image: &mut SparseImage, parent: &mut Dir, name: &str, len: usize, clusters: &[u32]) {
        assert_eq!(clusters.len(), len.div_ceil(self.cluster_bytes()), "{name}");
        self.link(image, clusters);
        self.add(
            image,
            parent,
            name,
            short_name(),
            0x20,
            clusters.first().copied().unwrap_or(0),
            len as u32,
        );
    }

    /// FSInfo's counts set to unknown, which is what the shrink leaves too.
    pub fn finish(&self, image: &mut SparseImage) {
        let mut s = image.sector(self.first + 1);
        s[488..496].fill(0xff);
        image.set(self.first + 1, &s);
        let mut b = image.sector(self.first + 7);
        if b[..4] == [0x52, 0x52, 0x61, 0x41] {
            b[488..496].fill(0xff);
            image.set(self.first + 7, &b);
        }
    }
}

fn short_name() -> [u8; 11] {
    use std::sync::atomic::{AtomicU32, Ordering};
    static NEXT: AtomicU32 = AtomicU32::new(0);
    let n = NEXT.fetch_add(1, Ordering::Relaxed);
    let mut s = [b' '; 11];
    s[..8].copy_from_slice(format!("T{n:07X}").as_bytes());
    s[8..].copy_from_slice(b"BIN");
    s
}

/// `n` clusters from `start` on.
pub fn run(start: u32, n: usize) -> Vec<u32> {
    (start..start + n as u32).collect()
}
