// SPDX-License-Identifier: GPL-3.0-or-later
//! Lazy chunk-cached FAT and allocation-bitmap readers for tolerant live-tree walks during `resume()`.

use std::collections::HashMap;

use switch_card::{Device, SECTOR};

use crate::apply::Geometry;
use crate::error::{Error, Result};
use crate::exfat::{BAD as EXFAT_BAD, EOC as EXFAT_EOC};
use crate::fat32::{BAD as FAT32_BAD, EOC as FAT32_EOC};
use crate::model::Bits;

const FAT_CHUNK_SECTORS: u64 = 64;
const BM_CHUNK_SECTORS: u64 = 16;

pub(super) struct FatReader {
    fat_lba: u64,
    fat_sectors: u64,
    mask: u32,
    chunks: HashMap<u64, Vec<u32>>,
}

impl FatReader {
    pub(super) fn new(fat_lba: u64, entries: u64, mask: u32) -> FatReader {
        FatReader {
            fat_lba,
            fat_sectors: (entries * 4).div_ceil(SECTOR as u64),
            mask,
            chunks: HashMap::new(),
        }
    }

    pub(super) fn get<D: Device>(&mut self, dev: &mut D, index: u32) -> Result<u32> {
        let sector = u64::from(index) / 128;
        let chunk_idx = sector / FAT_CHUNK_SECTORS;
        if let std::collections::hash_map::Entry::Vacant(e) = self.chunks.entry(chunk_idx) {
            let start = chunk_idx * FAT_CHUNK_SECTORS;
            let count = (self.fat_sectors - start).min(FAT_CHUNK_SECTORS);
            let mut buf = vec![0u8; count as usize * SECTOR];
            dev.read_sectors(self.fat_lba + start, &mut buf)?;
            let mask = self.mask;
            let vals: Vec<u32> = buf
                .as_chunks::<4>()
                .0
                .iter()
                .map(|&raw| u32::from_le_bytes(raw) & mask)
                .collect();
            e.insert(vals);
        }
        let within = (u64::from(index) - chunk_idx * FAT_CHUNK_SECTORS * 128) as usize;
        Ok(self.chunks[&chunk_idx][within])
    }

    pub(super) fn follow_exfat<D: Device>(&mut self, dev: &mut D, first: u32, end: u32) -> Result<Vec<u32>> {
        let mut chain = Vec::new();
        let mut c = first;
        let mut anchor = first;
        let mut power = 1u64;
        let mut steps = 0u64;
        loop {
            if c < 2 || c >= end {
                return Err(Error::Damaged(format!(
                    "a chain from cluster {first} leaves the volume"
                )));
            }
            chain.push(c);
            let next = self.get(dev, c)?;
            if next == EXFAT_EOC {
                return Ok(chain);
            }
            if next < 2 || next >= end || next == EXFAT_BAD {
                return Err(Error::Damaged(format!(
                    "a chain from cluster {first} hits an invalid FAT entry ({next:#x})"
                )));
            }
            if next == anchor {
                return Err(Error::Damaged(format!("a chain from cluster {first} loops")));
            }
            steps += 1;
            if steps == power {
                anchor = next;
                power *= 2;
                steps = 0;
            }
            c = next;
        }
    }

    pub(super) fn follow_fat32<D: Device>(&mut self, dev: &mut D, first: u32, end: u32) -> Result<Vec<u32>> {
        let mut chain = Vec::new();
        let mut c = first;
        let mut anchor = first;
        let mut power = 1u64;
        let mut steps = 0u64;
        loop {
            if c < 2 || c >= end {
                return Err(Error::Damaged(format!(
                    "a chain from cluster {first} leaves the volume"
                )));
            }
            chain.push(c);
            let next = self.get(dev, c)?;
            if next >= FAT32_EOC {
                return Ok(chain);
            }
            if next < 2 || next >= end || next == FAT32_BAD {
                return Err(Error::Damaged(format!(
                    "a chain from cluster {first} runs into a free or bad cluster"
                )));
            }
            if next == anchor {
                return Err(Error::Damaged(format!("a chain from cluster {first} loops")));
            }
            steps += 1;
            if steps == power {
                anchor = next;
                power *= 2;
                steps = 0;
            }
            c = next;
        }
    }
}

pub(super) struct BitmapReader<'a> {
    geo: &'a Geometry,
    chain: &'a [u32],
    sectors: HashMap<u64, [u8; SECTOR]>,
}

impl<'a> BitmapReader<'a> {
    pub(super) fn new(geo: &'a Geometry, chain: &'a [u32]) -> BitmapReader<'a> {
        BitmapReader {
            geo,
            chain,
            sectors: HashMap::new(),
        }
    }

    pub(super) fn get<D: Device>(&mut self, dev: &mut D, cluster: u32) -> Result<(u64, bool)> {
        let bit = u64::from(cluster - 2);
        let byte = bit / 8;
        let mask = 1u8 << (bit % 8);
        let cb = self.geo.cluster_bytes();
        let idx = (byte / cb) as usize;
        let Some(&bm_cluster) = self.chain.get(idx) else {
            return Err(Error::Damaged("cluster bit outside allocation bitmap".into()));
        };
        let within = byte % cb;
        let sec_in_cluster = within / SECTOR as u64;
        let cluster_base = self.geo.cluster_lba(bm_cluster);
        let lba = cluster_base + sec_in_cluster;
        if !self.sectors.contains_key(&lba) {
            let chunk_start = (sec_in_cluster / BM_CHUNK_SECTORS) * BM_CHUNK_SECTORS;
            let count = (self.geo.cluster_sectors - chunk_start).min(BM_CHUNK_SECTORS);
            let mut buf = vec![0u8; count as usize * SECTOR];
            dev.read_sectors(cluster_base + chunk_start, &mut buf)?;
            for (i, s) in buf.as_chunks::<SECTOR>().0.iter().enumerate() {
                self.sectors.insert(cluster_base + chunk_start + i as u64, *s);
            }
        }
        let off = (within % SECTOR as u64) as usize;
        Ok((lba, self.sectors[&lba][off] & mask != 0))
    }
}

pub(super) struct LiveMap {
    pub owned: Bits,
    pub fat_chained: Option<Bits>,
    pub fat: FatReader,
    pub new_cut: u32,
}

impl LiveMap {
    pub(super) fn is_fat_chained(&self, cluster: u32) -> bool {
        match &self.fat_chained {
            Some(bits) => bits.get(cluster),
            None => self.owned.get(cluster),
        }
    }
}
