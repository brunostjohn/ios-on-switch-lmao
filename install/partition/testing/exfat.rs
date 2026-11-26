// SPDX-License-Identifier: GPL-3.0-or-later
//! Files and directories put on an exFAT volume at exact clusters, supporting both `NoFatChain` and FAT-chained
//! streams with valid `NameHash`, `SetChecksum`, and Allocation Bitmap.

use std::collections::VecDeque;

use switch_card_testing::SparseImage;

use crate::fat32::content;

const EOC: u32 = 0xffff_ffff;

#[derive(Clone, Debug)]
pub struct Exfat {
    pub first: u64,
    pub cluster_sectors: u64,
    pub fat_offset: u64,
    pub fat_length: u64,
    pub heap_offset: u64,
    pub clusters: u32,
    pub root: u32,
    pub bitmap_first: u32,
    pub bitmap_data_len: u64,
    pub upcase_first: u32,
    upcase: Vec<u16>,
}

pub struct ExfatDir {
    pub first: u32,
    chain: Vec<u32>,
    slot: usize,
    pub spare: VecDeque<u32>,
}

impl Exfat {
    pub fn read(image: &SparseImage, first: u64) -> Exfat {
        let b = image.sector(first);
        let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
        let fat_offset = u64::from(u32_at(80));
        let fat_length = u64::from(u32_at(84));
        let heap_offset = u64::from(u32_at(88));
        let clusters = u32_at(92);
        let root = u32_at(96);
        let cluster_sectors = 1u64 << b[109];
        let mut vol = Exfat {
            first,
            cluster_sectors,
            fat_offset,
            fat_length,
            heap_offset,
            clusters,
            root,
            bitmap_first: 0,
            bitmap_data_len: 0,
            upcase_first: 0,
            upcase: Vec::new(),
        };
        let root_chain = vol.chain(image, root);
        let mut root_bytes = Vec::new();
        for c in root_chain {
            root_bytes.extend(image.read_bytes(vol.lba(c), cluster_sectors));
        }
        let mut up_len = 0u64;
        for e in root_bytes.as_chunks::<32>().0 {
            if e[0] == 0 {
                break;
            }
            if e[0] == 0x81 {
                vol.bitmap_first = u32::from_le_bytes(e[20..24].try_into().unwrap());
                vol.bitmap_data_len = u64::from_le_bytes(e[24..32].try_into().unwrap());
            } else if e[0] == 0x82 {
                vol.upcase_first = u32::from_le_bytes(e[20..24].try_into().unwrap());
                up_len = u64::from_le_bytes(e[24..32].try_into().unwrap());
            }
        }
        assert!(vol.bitmap_first >= 2 && vol.upcase_first >= 2);
        let mut up_raw = Vec::new();
        for c in vol.chain(image, vol.upcase_first) {
            up_raw.extend(image.read_bytes(vol.lba(c), cluster_sectors));
        }
        up_raw.truncate(up_len as usize);
        vol.upcase = decode_upcase(&up_raw);
        vol
    }

    pub fn cluster_bytes(&self) -> usize {
        self.cluster_sectors as usize * 512
    }

    pub fn lba(&self, cluster: u32) -> u64 {
        self.first + self.heap_offset + u64::from(cluster - 2) * self.cluster_sectors
    }

    fn fat_at(&self, cluster: u32) -> (u64, usize) {
        let byte = u64::from(cluster) * 4;
        (self.first + self.fat_offset + byte / 512, (byte % 512) as usize)
    }

    pub fn fat_get(&self, image: &SparseImage, cluster: u32) -> u32 {
        let (lba, at) = self.fat_at(cluster);
        u32::from_le_bytes(image.sector(lba)[at..at + 4].try_into().unwrap())
    }

    pub fn fat_set(&self, image: &mut SparseImage, cluster: u32, value: u32) {
        let (lba, at) = self.fat_at(cluster);
        let mut s = image.sector(lba);
        s[at..at + 4].copy_from_slice(&value.to_le_bytes());
        image.set(lba, &s);
    }

    pub fn chain(&self, image: &SparseImage, first: u32) -> Vec<u32> {
        let mut out = Vec::new();
        let mut c = first;
        loop {
            out.push(c);
            let next = self.fat_get(image, c);
            if next == EOC {
                return out;
            }
            c = next;
        }
    }

    fn bit_at(&self, cluster: u32) -> (u64, usize, u8) {
        let bit = u64::from(cluster - 2);
        let byte = bit / 8;
        let cb = self.cluster_bytes() as u64;
        let bm_cluster = self.bitmap_first + (byte / cb) as u32;
        let within = byte % cb;
        (
            self.lba(bm_cluster) + within / 512,
            (within % 512) as usize,
            1u8 << (bit % 8),
        )
    }

    pub fn bit_get(&self, image: &SparseImage, cluster: u32) -> bool {
        let (lba, at, mask) = self.bit_at(cluster);
        image.sector(lba)[at] & mask != 0
    }

    pub fn bit_set(&self, image: &mut SparseImage, cluster: u32, used: bool) {
        let (lba, at, mask) = self.bit_at(cluster);
        let mut s = image.sector(lba);
        if used {
            s[at] |= mask;
        } else {
            s[at] &= !mask;
        }
        image.set(lba, &s);
    }

    pub fn free(&self, image: &SparseImage, from: u32, n: usize) -> Vec<u32> {
        (from..self.clusters + 2)
            .filter(|&c| !self.bit_get(image, c))
            .take(n)
            .collect()
    }

    fn alloc(&self, image: &mut SparseImage, clusters: &[u32], no_fat_chain: bool) {
        if no_fat_chain {
            assert!(
                clusters.windows(2).all(|w| w[1] == w[0] + 1),
                "NoFatChain clusters must be contiguous"
            );
        }
        for (i, &c) in clusters.iter().enumerate() {
            assert!(!self.bit_get(image, c), "cluster {c} is already allocated");
            self.bit_set(image, c, true);
            if !no_fat_chain {
                self.fat_set(image, c, clusters.get(i + 1).copied().unwrap_or(EOC));
            }
        }
    }

    pub fn root_dir(&self, image: &SparseImage, spare: Vec<u32>) -> ExfatDir {
        let chain = self.chain(image, self.root);
        let per = self.cluster_bytes() / 32;
        let mut slot = 0;
        'scan: for &c in &chain {
            let bytes = image.read_bytes(self.lba(c), self.cluster_sectors);
            for e in bytes.as_chunks::<32>().0 {
                if e[0] == 0 {
                    break 'scan;
                }
                slot += 1;
            }
        }
        assert!(slot <= chain.len() * per);
        ExfatDir {
            first: self.root,
            chain,
            slot,
            spare: spare.into(),
        }
    }

    pub fn slot_in_sector(&self, dir: &ExfatDir) -> usize {
        dir.slot % 16
    }

    fn entry_slot(&self, image: &mut SparseImage, dir: &mut ExfatDir) -> (u64, usize) {
        let per = self.cluster_bytes() / 32;
        if dir.slot == dir.chain.len() * per {
            let next = dir
                .spare
                .pop_front()
                .expect("the root directory needs a spare cluster to grow into");
            assert!(!self.bit_get(image, next), "cluster {next} is taken");
            self.bit_set(image, next, true);
            self.fat_set(image, *dir.chain.last().unwrap(), next);
            self.fat_set(image, next, EOC);
            image.set(self.lba(next), &vec![0; self.cluster_bytes()]);
            dir.chain.push(next);
        }
        let cluster = dir.chain[dir.slot / per];
        let within = (dir.slot % per) * 32;
        dir.slot += 1;
        (self.lba(cluster) + within as u64 / 512, within % 512)
    }

    fn put_entry(&self, image: &mut SparseImage, dir: &mut ExfatDir, entry: &[u8; 32]) {
        let (lba, at) = self.entry_slot(image, dir);
        let mut s = image.sector(lba);
        s[at..at + 32].copy_from_slice(entry);
        image.set(lba, &s);
    }

    #[allow(clippy::too_many_arguments)]
    fn add_set(
        &self,
        image: &mut SparseImage,
        dir: &mut ExfatDir,
        name: &str,
        attr: u16,
        first: u32,
        len: u64,
        no_fat_chain: bool,
    ) {
        let units: Vec<u16> = name.encode_utf16().collect();
        let name_entries = units.len().div_ceil(15);
        let secondary = 1 + name_entries;
        let mut set = vec![0u8; (1 + secondary) * 32];

        set[0] = 0x85;
        set[1] = secondary as u8;
        set[4..6].copy_from_slice(&attr.to_le_bytes());
        let ts = ((2026u32 - 1980) << 25 | 11 << 21 | 26 << 16).to_le_bytes();
        for at in [8, 12, 16] {
            set[at..at + 4].copy_from_slice(&ts);
        }

        let stream = &mut set[32..64];
        stream[0] = 0xc0;
        stream[1] = 0x01 | if no_fat_chain { 0x02 } else { 0x00 };
        stream[3] = units.len() as u8;
        stream[4..6].copy_from_slice(&self.name_hash(&units).to_le_bytes());
        stream[8..16].copy_from_slice(&len.to_le_bytes());
        stream[20..24].copy_from_slice(&first.to_le_bytes());
        stream[24..32].copy_from_slice(&len.to_le_bytes());

        for k in 0..name_entries {
            let ne = &mut set[(2 + k) * 32..(3 + k) * 32];
            ne[0] = 0xc1;
            for j in 0..15 {
                let idx = k * 15 + j;
                let u = units.get(idx).copied().unwrap_or(0);
                ne[2 + j * 2..4 + j * 2].copy_from_slice(&u.to_le_bytes());
            }
        }

        let sum = entry_set_checksum(&set);
        set[2..4].copy_from_slice(&sum.to_le_bytes());
        for chunk in set.as_chunks::<32>().0 {
            self.put_entry(image, dir, chunk);
        }
    }

    fn name_hash(&self, units: &[u16]) -> u16 {
        let mut hash = 0u16;
        for &u in units {
            let up = self.upcase.get(u as usize).copied().unwrap_or(u);
            for b in up.to_le_bytes() {
                hash = hash.rotate_right(1).wrapping_add(u16::from(b));
            }
        }
        hash
    }

    /// A subdirectory pre-allocated across all of `clusters`.
    pub fn mkdir(
        &self,
        image: &mut SparseImage,
        parent: &mut ExfatDir,
        name: &str,
        clusters: Vec<u32>,
        no_fat_chain: bool,
    ) -> ExfatDir {
        assert!(!clusters.is_empty());
        self.alloc(image, &clusters, no_fat_chain);
        for &c in &clusters {
            image.set(self.lba(c), &vec![0; self.cluster_bytes()]);
        }
        let first = clusters[0];
        let len = clusters.len() as u64 * self.cluster_bytes() as u64;
        self.add_set(image, parent, name, 0x0010, first, len, no_fat_chain);
        ExfatDir {
            first,
            chain: clusters,
            slot: 0,
            spare: VecDeque::new(),
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn file(
        &self,
        image: &mut SparseImage,
        parent: &mut ExfatDir,
        name: &str,
        len: usize,
        clusters: &[u32],
        seed: u64,
        no_fat_chain: bool,
    ) {
        let need = len.div_ceil(self.cluster_bytes());
        assert_eq!(clusters.len(), need, "{name}: {need} clusters");
        self.alloc(image, clusters, no_fat_chain);
        let data = content(seed, len);
        let cb = self.cluster_bytes();
        for (i, &c) in clusters.iter().enumerate() {
            let mut chunk = data[i * cb..((i + 1) * cb).min(len)].to_vec();
            chunk.resize(cb, 0);
            image.set(self.lba(c), &chunk);
        }
        let first = clusters.first().copied().unwrap_or(0);
        let nfc = if len == 0 { true } else { no_fat_chain };
        self.add_set(image, parent, name, 0x0020, first, len as u64, nfc);
    }

    pub fn file_zero(
        &self,
        image: &mut SparseImage,
        parent: &mut ExfatDir,
        name: &str,
        len: usize,
        clusters: &[u32],
        no_fat_chain: bool,
    ) {
        assert_eq!(clusters.len(), len.div_ceil(self.cluster_bytes()), "{name}");
        self.alloc(image, clusters, no_fat_chain);
        let first = clusters.first().copied().unwrap_or(0);
        let nfc = if len == 0 { true } else { no_fat_chain };
        self.add_set(image, parent, name, 0x0020, first, len as u64, nfc);
    }
}

pub fn entry_set_checksum(entries: &[u8]) -> u16 {
    let mut sum = 0u16;
    for (i, &b) in entries.iter().enumerate() {
        if i == 2 || i == 3 {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u16::from(b));
    }
    sum
}

pub fn decode_upcase(raw: &[u8]) -> Vec<u16> {
    let mut table: Vec<u16> = (0..=u16::MAX).collect();
    let mut cp = 0usize;
    let mut i = 0usize;
    while i + 2 <= raw.len() && cp < table.len() {
        let v = u16::from_le_bytes([raw[i], raw[i + 1]]);
        i += 2;
        if v == 0xffff && i + 2 <= raw.len() {
            let skip = u16::from_le_bytes([raw[i], raw[i + 1]]) as usize;
            i += 2;
            cp += skip;
        } else {
            table[cp] = v;
            cp += 1;
        }
    }
    table
}
