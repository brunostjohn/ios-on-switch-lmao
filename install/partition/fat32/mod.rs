// SPDX-License-Identifier: GPL-3.0-or-later
//! A FAT32 boot volume as the shrink sees it: its boot sector and its whole FAT, held in memory and kept current as
//! steps apply.

pub mod boot;
pub mod relocate;
pub mod walk;

use switch_card::{Device, SECTOR};

use crate::apply::Geometry;
use crate::cache::MAX_REQUEST;
use crate::error::{Error, Result};
use boot::Bpb;

pub const MASK: u32 = 0x0fff_ffff;
pub const BAD: u32 = 0x0fff_fff7;
pub const EOC: u32 = 0x0fff_fff8;

pub struct Fat32 {
    pub first: u64,
    pub bpb: Bpb,
    /// Every entry, masked to 28 bits, for clusters below `cluster_end`.
    pub fat: Vec<u32>,
}

impl Fat32 {
    pub fn load<D: Device>(dev: &mut D, first: u64, partition_sectors: u64) -> Result<Fat32> {
        let mut boot = [0u8; SECTOR];
        dev.read_sectors(first, &mut boot)?;
        let bpb = Bpb::parse(&boot, partition_sectors)?;
        if bpb.backup_boot != 0 {
            let mut backup = [0u8; SECTOR];
            dev.read_sectors(first + bpb.backup_boot, &mut backup)?;
            let backup_bpb = Bpb::parse(&backup, partition_sectors)
                .map_err(|_| Error::Damaged("the backup boot sector is invalid".into()))?;
            if backup_bpb.total != bpb.total || backup_bpb.root != bpb.root {
                return Err(Error::Damaged(
                    "the backup boot sector disagrees with the main one".into(),
                ));
            }
        }
        let end = bpb.clusters() + 2;
        let fat = read_fat(dev, first + bpb.reserved, end)?;
        if fat[1] & 0x0800_0000 == 0 || fat[1] & 0x0400_0000 == 0 {
            return Err(Error::Damaged(
                "the volume was not cleanly unmounted or had I/O errors".into(),
            ));
        }
        for copy in 1..bpb.fats {
            if read_fat(dev, first + bpb.reserved + copy * bpb.fat_sectors, end)? != fat {
                return Err(Error::Damaged("the FAT's two copies differ".into()));
            }
        }
        if fat[2..].contains(&BAD) {
            return Err(Error::Damaged("the volume has bad clusters".into()));
        }
        Ok(Fat32 { first, bpb, fat })
    }

    pub fn cluster_end(&self) -> u32 {
        self.fat.len() as u32
    }

    /// The first cluster past a volume of `total` sectors: every cluster from here on is cut away.
    pub fn cut(&self, total: u64) -> u32 {
        (self.bpb.clusters_in(total) + 2).min(self.fat.len() as u64) as u32
    }

    pub fn geometry(&self) -> Geometry {
        geometry(self.first, &self.bpb)
    }

    pub fn is_used(&self, cluster: u32) -> bool {
        let v = self.fat[cluster as usize];
        v != 0 && v != BAD
    }

    /// The chain from `first`, checked to stay inside the volume and to end.
    pub fn chain(&self, first: u32) -> Result<Vec<u32>> {
        let mut chain = Vec::new();
        let mut c = first;
        let mut anchor = first;
        let mut power = 1u64;
        let mut steps = 0u64;
        loop {
            if c < 2 || c >= self.cluster_end() {
                return Err(Error::Damaged(format!(
                    "a chain from cluster {first} leaves the volume"
                )));
            }
            chain.push(c);
            let next = self.fat[c as usize];
            if next >= EOC {
                return Ok(chain);
            }
            if next == 0 || next == BAD {
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

pub fn geometry(first: u64, bpb: &Bpb) -> Geometry {
    Geometry {
        fat_lbas: (0..bpb.fats)
            .map(|c| first + bpb.reserved + c * bpb.fat_sectors)
            .collect(),
        fat_mask: MASK,
        heap_lba: first + bpb.data_start(),
        cluster_sectors: bpb.cluster_sectors,
        cluster_end: bpb.clusters() as u32 + 2,
    }
}

fn read_fat<D: Device>(dev: &mut D, lba: u64, entries: u64) -> Result<Vec<u32>> {
    let sectors = (entries * 4).div_ceil(SECTOR as u64);
    let mut fat = Vec::with_capacity(entries as usize);
    let mut buf = vec![0u8; MAX_REQUEST as usize * SECTOR];
    let mut done = 0;
    while done < sectors {
        let n = (sectors - done).min(MAX_REQUEST);
        let chunk = &mut buf[..n as usize * SECTOR];
        dev.read_sectors(lba + done, chunk)?;
        fat.extend(
            chunk
                .chunks(4)
                .map(|e| u32::from_le_bytes(e.try_into().unwrap()) & MASK),
        );
        done += n;
    }
    fat.truncate(entries as usize);
    Ok(fat)
}
