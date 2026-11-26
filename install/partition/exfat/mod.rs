// SPDX-License-Identifier: GPL-3.0-or-later
//! An exFAT boot volume as the shrink sees it: its boot region, FAT, allocation bitmap, and up-case table, kept
//! current as relocation steps apply.

pub mod boot;
pub mod relocate;
pub mod shrink;
pub mod walk;

use switch_card::{Device, SECTOR};

use crate::apply::Geometry;
use crate::cache::MAX_REQUEST;
use crate::error::{Error, Result};
use boot::{REGION_SECTORS, Vbr};

pub const BAD: u32 = 0xffff_fff7;
pub const EOC: u32 = 0xffff_ffff;
const MAX_UPCASE_BYTES: u64 = 131_072;

pub struct Exfat {
    pub first: u64,
    pub vbr: Vbr,
    pub fat: Vec<u32>,
    pub bitmap: Vec<u8>,
    pub bitmap_chain: Vec<u32>,
    pub bitmap_entry_offset: u64,
    pub bitmap_data_len: u64,
    pub upcase_chain: Vec<u32>,
}

impl Exfat {
    pub fn load<D: Device>(dev: &mut D, first: u64, partition_sectors: u64) -> Result<Exfat> {
        if partition_sectors < boot::MIN_VOLUME_SECTORS {
            return Err(Error::Volume(
                "exFAT boot sector: partition is smaller than 1 MiB".into(),
            ));
        }
        let mut regions = vec![0u8; (REGION_SECTORS * 2) as usize * SECTOR];
        dev.read_sectors(first, &mut regions)?;
        let (main, backup) = regions.split_at(REGION_SECTORS as usize * SECTOR);
        let vbr = Vbr::parse(main[..SECTOR].try_into().unwrap(), partition_sectors)?;
        boot::verify_regions(main, backup)?;

        let end = u64::from(vbr.cluster_count) + 2;
        let fat = read_fat(dev, first + vbr.fat_offset, end)?;
        if fat[0] != 0xffff_fff8 || fat[1] != EOC {
            return Err(Error::Damaged("exFAT FAT[0..2] header entries are invalid".into()));
        }
        if fat[2..].contains(&BAD) {
            return Err(Error::Damaged("the volume has bad clusters".into()));
        }

        let geo = base_geometry(first, &vbr, Vec::new());
        let root_chain = follow_chain(&fat, vbr.root)?;
        let root_data = read_clusters(dev, &geo, &root_chain)?;

        let mut bm_entry: Option<(u64, u32, u64)> = None;
        let mut up_entry: Option<(u32, u32, u64)> = None;
        for (i, e) in root_data.as_chunks::<32>().0.iter().enumerate() {
            match e[0] {
                0x00 => break,
                0x81 => {
                    if e[1] != 0 {
                        return Err(Error::Volume("second allocation bitmap (TexFAT) is unsupported".into()));
                    }
                    if bm_entry.is_some() {
                        return Err(Error::Damaged("duplicate allocation bitmap entry".into()));
                    }
                    bm_entry = Some((
                        i as u64 * 32,
                        u32::from_le_bytes(e[20..24].try_into().unwrap()),
                        u64::from_le_bytes(e[24..32].try_into().unwrap()),
                    ));
                }
                0x82 => {
                    if up_entry.is_some() {
                        return Err(Error::Damaged("duplicate up-case table entry".into()));
                    }
                    up_entry = Some((
                        u32::from_le_bytes(e[4..8].try_into().unwrap()),
                        u32::from_le_bytes(e[20..24].try_into().unwrap()),
                        u64::from_le_bytes(e[24..32].try_into().unwrap()),
                    ));
                }
                _ => {}
            }
        }

        let (bitmap_entry_offset, bm_first, bitmap_data_len) =
            bm_entry.ok_or_else(|| Error::Damaged("missing allocation bitmap entry".into()))?;
        let min_bm_len = u64::from(vbr.cluster_count).div_ceil(8);
        if bitmap_data_len < min_bm_len {
            return Err(Error::Damaged("allocation bitmap is shorter than ClusterCount".into()));
        }
        let bitmap_chain = follow_chain(&fat, bm_first)?;
        if bitmap_chain.len() as u64 != bitmap_data_len.div_ceil(vbr.cluster_bytes()) {
            return Err(Error::Damaged(
                "allocation bitmap chain length disagrees with DataLength".into(),
            ));
        }
        if !bitmap_chain.windows(2).all(|w| w[1] == w[0] + 1) {
            return Err(Error::Volume("fragmented allocation bitmap is not supported".into()));
        }
        let mut bitmap = read_clusters(dev, &geo, &bitmap_chain)?;
        bitmap.truncate(min_bm_len as usize);

        let (up_checksum, up_first, up_len) =
            up_entry.ok_or_else(|| Error::Damaged("missing up-case table entry".into()))?;
        if up_len == 0 || up_len > MAX_UPCASE_BYTES || !up_len.is_multiple_of(2) {
            return Err(Error::Damaged("invalid up-case table length".into()));
        }
        let upcase_chain = follow_chain(&fat, up_first)?;
        if upcase_chain.len() as u64 != up_len.div_ceil(vbr.cluster_bytes()) {
            return Err(Error::Damaged(
                "up-case table chain length disagrees with DataLength".into(),
            ));
        }
        let up_bytes = read_clusters(dev, &geo, &upcase_chain)?;
        if table_checksum(&up_bytes[..up_len as usize]) != up_checksum {
            return Err(Error::Damaged("up-case table checksum is invalid".into()));
        }

        Ok(Exfat {
            first,
            vbr,
            fat,
            bitmap,
            bitmap_chain,
            bitmap_entry_offset,
            bitmap_data_len,
            upcase_chain,
        })
    }

    pub fn cluster_end(&self) -> u32 {
        self.vbr.cluster_count + 2
    }

    /// One past the highest cluster occupied by the allocation bitmap, up-case table, or root directory's first cluster.
    pub fn system_end(&self) -> u32 {
        let bm_max = self.bitmap_chain.iter().copied().max().unwrap_or(2);
        let up_max = self.upcase_chain.iter().copied().max().unwrap_or(2);
        bm_max.max(up_max).max(self.vbr.root) + 1
    }

    pub fn cut(&self, total: u64) -> u32 {
        (self.vbr.clusters_in(total) + 2).min(u64::from(self.cluster_end())) as u32
    }

    pub fn geometry(&self) -> Geometry {
        base_geometry(self.first, &self.vbr, self.bitmap_chain.clone())
    }

    pub fn is_used(&self, cluster: u32) -> bool {
        let bit = (cluster - 2) as usize;
        (self.bitmap[bit / 8] >> (bit % 8)) & 1 != 0
    }

    pub fn set_used(&mut self, cluster: u32, used: bool) {
        let bit = (cluster - 2) as usize;
        let mask = 1u8 << (bit % 8);
        if used {
            self.bitmap[bit / 8] |= mask;
        } else {
            self.bitmap[bit / 8] &= !mask;
        }
    }

    pub fn chain(&self, first: u32) -> Result<Vec<u32>> {
        follow_chain(&self.fat, first)
    }

    pub fn stream_clusters(&self, first: u32, cluster_count: u32, no_fat_chain: bool) -> Result<Vec<u32>> {
        if no_fat_chain {
            let end = first
                .checked_add(cluster_count)
                .filter(|&e| first >= 2 && e <= self.cluster_end())
                .ok_or_else(|| Error::Damaged(format!("contiguous stream from cluster {first} leaves the volume")))?;
            Ok((first..end).collect())
        } else {
            self.chain(first)
        }
    }
}

/// Lightweight geometry lookup for `apply`/`resume`: tolerates mid-shrink bitmap length and FAT chain transitions.
pub fn geometry<D: Device>(dev: &mut D, first: u64, partition_sectors: u64) -> Result<Geometry> {
    if partition_sectors < boot::MIN_VOLUME_SECTORS {
        return Err(Error::Volume(
            "exFAT boot sector: partition is smaller than 1 MiB".into(),
        ));
    }
    let mut sector = [0u8; SECTOR];
    dev.read_sectors(first, &mut sector)?;
    let vbr = Vbr::parse(&sector, partition_sectors)?;
    let geo = base_geometry(first, &vbr, Vec::new());
    let root_chain = read_chain_from_dev(dev, &geo, vbr.root)?;
    let root_data = read_clusters(dev, &geo, &root_chain)?;
    let mut found = None;
    for e in root_data.as_chunks::<32>().0 {
        if e[0] == 0x00 {
            break;
        }
        if e[0] == 0x81 && e[1] == 0 {
            let bm_first = u32::from_le_bytes(e[20..24].try_into().unwrap());
            let bm_len = u64::from_le_bytes(e[24..32].try_into().unwrap());
            found = Some((bm_first, bm_len));
            break;
        }
    }
    let (bm_first, bm_len) = found.ok_or_else(|| Error::Damaged("missing allocation bitmap entry".into()))?;
    let bm_chain = read_chain_from_dev(dev, &geo, bm_first)?;
    let need = bm_len.div_ceil(vbr.cluster_bytes());
    if (bm_chain.len() as u64) < need {
        return Err(Error::Damaged("allocation bitmap chain shorter than DataLength".into()));
    }
    Ok(base_geometry(first, &vbr, bm_chain))
}

fn base_geometry(first: u64, vbr: &Vbr, bitmap: Vec<u32>) -> Geometry {
    Geometry {
        fat_lbas: vec![first + vbr.fat_offset],
        fat_mask: u32::MAX,
        heap_lba: first + vbr.heap_offset,
        cluster_sectors: vbr.cluster_sectors,
        cluster_end: vbr.cluster_count + 2,
        bitmap,
    }
}

pub fn table_checksum(data: &[u8]) -> u32 {
    let mut sum = 0u32;
    for &b in data {
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    sum
}

pub fn follow_chain(fat: &[u32], first: u32) -> Result<Vec<u32>> {
    let end = fat.len() as u32;
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
        let next = fat[c as usize];
        if next == EOC {
            return Ok(chain);
        }
        if next < 2 || next >= end || next == BAD {
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

fn read_chain_from_dev<D: Device>(dev: &mut D, geo: &Geometry, first: u32) -> Result<Vec<u32>> {
    let mut chain = Vec::new();
    let mut c = first;
    let mut anchor = first;
    let mut power = 1u64;
    let mut steps = 0u64;
    let mut cached_lba = u64::MAX;
    let mut buf = [0u8; SECTOR];
    loop {
        if c < 2 || c >= geo.cluster_end {
            return Err(Error::Damaged(format!(
                "a chain from cluster {first} leaves the volume"
            )));
        }
        chain.push(c);
        let byte = u64::from(c) * 4;
        let lba = geo.fat_lbas[0] + byte / SECTOR as u64;
        if lba != cached_lba {
            dev.read_sectors(lba, &mut buf)?;
            cached_lba = lba;
        }
        let at = (byte % SECTOR as u64) as usize;
        let next = u32::from_le_bytes(buf[at..at + 4].try_into().unwrap());
        if next == EOC {
            return Ok(chain);
        }
        if next < 2 || next >= geo.cluster_end || next == BAD {
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

fn read_fat<D: Device>(dev: &mut D, lba: u64, entries: u64) -> Result<Vec<u32>> {
    let sectors = (entries * 4).div_ceil(SECTOR as u64);
    let mut fat = Vec::with_capacity(entries as usize);
    let mut buf = vec![0u8; MAX_REQUEST as usize * SECTOR];
    let mut done = 0;
    while done < sectors {
        let n = (sectors - done).min(MAX_REQUEST);
        let chunk = &mut buf[..n as usize * SECTOR];
        dev.read_sectors(lba + done, chunk)?;
        fat.extend(chunk.as_chunks::<4>().0.iter().map(|&e| u32::from_le_bytes(e)));
        done += n;
    }
    fat.truncate(entries as usize);
    Ok(fat)
}

pub fn read_clusters<D: Device>(dev: &mut D, geo: &Geometry, chain: &[u32]) -> Result<Vec<u8>> {
    let per = geo.cluster_sectors;
    let mut data = vec![0u8; chain.len() * (per as usize) * SECTOR];
    let mut i = 0;
    while i < chain.len() {
        let mut n = 1;
        while i + n < chain.len() && chain[i + n] == chain[i] + n as u32 {
            n += 1;
        }
        let lba = geo.cluster_lba(chain[i]);
        let at = i * per as usize * SECTOR;
        let total_sectors = n as u64 * per;
        let mut done = 0u64;
        while done < total_sectors {
            let chunk = (total_sectors - done).min(MAX_REQUEST);
            let byte_off = at + done as usize * SECTOR;
            let byte_len = chunk as usize * SECTOR;
            dev.read_sectors(lba + done, &mut data[byte_off..byte_off + byte_len])?;
            done += chunk;
        }
        i += n;
    }
    Ok(data)
}
