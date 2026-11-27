// SPDX-License-Identifier: GPL-3.0-or-later
//! Applying a journaled step to the card: each group's sectors read, every value checked to be the record's old or
//! new one, the changed sectors written in long runs, then one barrier before the next group.

use switch_card::{Device, SECTOR};

use crate::cache::Cache;
use crate::error::{Error, Result};
use crate::journal::{FieldKind, Record, Step};
use crate::precheck::precheck;

pub(crate) const FAT32_EOC: u32 = 0x0fff_fff8;

/// Where a volume's FAT, clusters, and (for exFAT) allocation bitmap sit, for turning records into sectors.
#[derive(Clone, Debug)]
pub struct Geometry {
    pub fat_lbas: Vec<u64>,
    pub fat_mask: u32,
    pub heap_lba: u64,
    pub cluster_sectors: u64,
    /// One past the highest cluster number in use.
    pub cluster_end: u32,
    /// exFAT allocation bitmap cluster chain (empty for FAT32).
    pub bitmap: Vec<u32>,
}

impl Geometry {
    pub fn cluster_lba(&self, cluster: u32) -> u64 {
        self.heap_lba + u64::from(cluster - 2) * self.cluster_sectors
    }

    pub(crate) fn cluster_of_lba(&self, lba: u64) -> Option<u32> {
        let rel = lba.checked_sub(self.heap_lba)?;
        let cluster = 2u64.checked_add(rel / self.cluster_sectors)?;
        (cluster < u64::from(self.cluster_end)).then_some(cluster as u32)
    }

    pub fn cluster_bytes(&self) -> u64 {
        self.cluster_sectors * SECTOR as u64
    }

    fn fat_entry(&self, copy: usize, index: u32) -> (u64, usize) {
        let byte = u64::from(index) * 4;
        (
            self.fat_lbas[copy] + byte / SECTOR as u64,
            (byte % SECTOR as u64) as usize,
        )
    }

    pub(crate) fn bitmap_pos(&self, cluster: u32) -> Result<(u64, usize, u8)> {
        if cluster < 2 || cluster >= self.cluster_end {
            return Err(Error::Journal("bitmap record out of volume bounds"));
        }
        let bit = u64::from(cluster - 2);
        let byte = bit / 8;
        let mask = 1u8 << (bit % 8);
        let cb = self.cluster_bytes();
        let idx = (byte / cb) as usize;
        let Some(&bm_cluster) = self.bitmap.get(idx) else {
            return Err(Error::Journal("bitmap bit outside allocation bitmap"));
        };
        let within = byte % cb;
        Ok((
            self.cluster_lba(bm_cluster) + within / SECTOR as u64,
            (within % SECTOR as u64) as usize,
            mask,
        ))
    }

    pub(crate) fn bitmap_ranges(&self, ranges: &mut Vec<(u64, u64)>, first: u32, len: u32) {
        if len == 0 || self.bitmap.is_empty() || first < 2 {
            return;
        }
        let Some(last) = first.checked_add(len - 1) else {
            return;
        };
        let start_byte = u64::from(first - 2) / 8;
        let end_byte = u64::from(last - 2) / 8;
        let cb = self.cluster_bytes();
        let c0 = (start_byte / cb) as usize;
        let c1 = (end_byte / cb) as usize;
        for ci in c0..=c1 {
            let Some(&bm_cluster) = self.bitmap.get(ci) else {
                break;
            };
            let b0 = if ci == c0 { start_byte % cb } else { 0 };
            let b1 = if ci == c1 { end_byte % cb } else { cb - 1 };
            let s0 = b0 / SECTOR as u64;
            let s1 = b1 / SECTOR as u64;
            ranges.push((self.cluster_lba(bm_cluster) + s0, s1 - s0 + 1));
        }
    }
}

/// Applies every group of `step` in order; the number of write requests made.
pub fn apply<D: Device>(dev: &mut D, geo: &Geometry, step: &Step) -> Result<usize> {
    precheck(dev, geo, step)?;
    apply_checked(dev, geo, step)
}

pub(crate) fn apply_checked<D: Device>(dev: &mut D, geo: &Geometry, step: &Step) -> Result<usize> {
    let mut writes = 0;
    for group in &step.groups {
        let mut cache = Cache::default();
        cache.load(dev, prefetch(geo, group))?;
        for record in group {
            apply_record(dev, geo, &mut cache, record)?;
        }
        let made = cache.flush(dev)?;
        if made > 0 {
            dev.barrier()?;
        }
        writes += made;
    }
    Ok(writes)
}

pub(crate) fn check_field_liveness(sector: &[u8; SECTOR], at: usize, kind: FieldKind, lba: u64) -> Result<()> {
    match kind {
        FieldKind::Fat32Cluster if sector[at] == 0x00 || sector[at] == 0xe5 => {
            Err(changed(lba, "a deleted or free directory entry"))
        }
        FieldKind::ExfatChecksum if sector[at] != 0x85 => Err(changed(lba, "a deleted or replaced exFAT file entry")),
        FieldKind::ExfatStreamFlags | FieldKind::ExfatStreamCluster if sector[at] != 0xc0 => {
            Err(changed(lba, "a deleted or replaced exFAT stream entry"))
        }
        FieldKind::ExfatBitmapLen if sector[at] != 0x81 => {
            Err(changed(lba, "a missing or replaced exFAT bitmap entry"))
        }
        _ => Ok(()),
    }
}

pub(crate) fn prefetch(geo: &Geometry, group: &[Record]) -> Vec<(u64, u64)> {
    let mut ranges = Vec::new();
    let fat_range = |ranges: &mut Vec<(u64, u64)>, first: u32, len: u32| {
        if first.checked_add(len - 1).is_none() {
            return;
        }
        for copy in 0..geo.fat_lbas.len() {
            let (a, _) = geo.fat_entry(copy, first);
            let (b, _) = geo.fat_entry(copy, first + len - 1);
            ranges.push((a, b - a + 1));
        }
    };
    for r in group {
        match *r {
            Record::Chain { first, len, .. } | Record::Free { first, len, .. } if len > 0 => {
                fat_range(&mut ranges, first, len)
            }
            Record::Fat { index, .. } => fat_range(&mut ranges, index, 1),
            Record::Field { lba, .. } | Record::Sector { lba, .. } => ranges.push((lba, 1)),
            Record::Zero { lba, count } if count > 0 => ranges.push((lba, u64::from(count))),
            Record::Bits { first, len, .. } if len > 0 => geo.bitmap_ranges(&mut ranges, first, len),
            _ => {}
        }
    }
    ranges
}

pub(crate) fn changed(lba: u64, what: &'static str) -> Error {
    Error::Changed { lba, what }
}

fn apply_record<D: Device>(dev: &mut D, geo: &Geometry, cache: &mut Cache, record: &Record) -> Result<()> {
    match *record {
        Record::Chain {
            first,
            len,
            last,
            loose,
        } => {
            for i in 0..len {
                let new = if i + 1 < len { first + i + 1 } else { last };
                set_fat(dev, geo, cache, first + i, (!loose).then_some(0), new)?;
            }
        }
        Record::Free { first, len, last } => {
            for i in 0..len {
                let old = if i + 1 < len { first + i + 1 } else { last };
                set_fat(dev, geo, cache, first + i, Some(old), 0)?;
            }
        }
        Record::Fat { index, old, new } => set_fat(dev, geo, cache, index, Some(old), new)?,
        Record::Field {
            lba,
            offset,
            kind,
            old,
            new,
        } => {
            let sector = cache.get(dev, lba)?;
            let at = offset as usize;
            check_field_liveness(sector, at, kind, lba)?;
            let current = get_field(sector, at, kind);
            if current != new {
                if current != old {
                    return Err(changed(lba, "a directory entry or boot sector field"));
                }
                set_field(sector, at, kind, new);
            }
        }
        Record::DotDot { old, new, root } => dot_dot(dev, geo, cache, old, new, root)?,
        Record::Zero { lba, count } => {
            for i in 0..u64::from(count) {
                cache.get(dev, lba + i)?.fill(0);
            }
        }
        Record::Sector { lba, old, ref new } => {
            let sector = cache.get(dev, lba)?;
            if sector != &**new {
                if crc32fast::hash(&sector[..]) != old {
                    return Err(changed(lba, "a partition table or boot sector"));
                }
                *sector = **new;
            }
        }
        Record::Bits { first, len, set } => {
            for i in 0..len {
                let (lba, off, mask) = geo.bitmap_pos(first + i)?;
                let sector = cache.get(dev, lba)?;
                if set {
                    sector[off] |= mask;
                } else {
                    sector[off] &= !mask;
                }
            }
        }
    }
    Ok(())
}

pub(crate) fn fat_read<D: Device>(
    dev: &mut D,
    geo: &Geometry,
    cache: &mut Cache,
    copy: usize,
    index: u32,
) -> Result<(u64, u32)> {
    let (lba, at) = geo.fat_entry(copy, index);
    let sector = cache.get(dev, lba)?;
    let raw = u32::from_le_bytes(sector[at..at + 4].try_into().unwrap());
    Ok((lba, raw & geo.fat_mask))
}

fn set_fat<D: Device>(
    dev: &mut D,
    geo: &Geometry,
    cache: &mut Cache,
    index: u32,
    old: Option<u32>,
    new: u32,
) -> Result<()> {
    for copy in 0..geo.fat_lbas.len() {
        let (lba, at) = geo.fat_entry(copy, index);
        let sector = cache.get(dev, lba)?;
        let raw = u32::from_le_bytes(sector[at..at + 4].try_into().unwrap());
        let current = raw & geo.fat_mask;
        if current == new {
            continue;
        }
        if old.is_some_and(|o| o != current) {
            return Err(changed(lba, "a FAT entry"));
        }
        let value = (raw & !geo.fat_mask) | new;
        sector[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    Ok(())
}

fn fat_value<D: Device>(dev: &mut D, geo: &Geometry, cache: &mut Cache, index: u32) -> Result<u32> {
    let (_, value) = fat_read(dev, geo, cache, 0, index)?;
    Ok(value)
}

pub fn get_field(s: &[u8; SECTOR], at: usize, kind: FieldKind) -> u64 {
    let le = |pos: usize, n: usize| {
        let mut v = [0u8; 8];
        v[..n].copy_from_slice(&s[pos..pos + n]);
        u64::from_le_bytes(v)
    };
    match kind {
        FieldKind::Le8 => le(at, 1),
        FieldKind::Le16 => le(at, 2),
        FieldKind::Le32 => le(at, 4),
        FieldKind::Le64 => le(at, 8),
        FieldKind::Fat32Cluster => {
            let half = |o: usize| u64::from(u16::from_le_bytes([s[at + o], s[at + o + 1]]));
            half(20) << 16 | half(26)
        }
        FieldKind::ExfatChecksum => le(at + 2, 2),
        FieldKind::ExfatStreamFlags => le(at + 1, 1),
        FieldKind::ExfatStreamCluster => le(at + 20, 4),
        FieldKind::ExfatBitmapLen => le(at + 24, 8),
    }
}

pub fn set_field(s: &mut [u8; SECTOR], at: usize, kind: FieldKind, v: u64) {
    let bytes = v.to_le_bytes();
    match kind {
        FieldKind::Le8 => s[at] = bytes[0],
        FieldKind::Le16 => s[at..at + 2].copy_from_slice(&bytes[..2]),
        FieldKind::Le32 => s[at..at + 4].copy_from_slice(&bytes[..4]),
        FieldKind::Le64 => s[at..at + 8].copy_from_slice(&bytes),
        FieldKind::Fat32Cluster => {
            s[at + 20..at + 22].copy_from_slice(&((v >> 16) as u16).to_le_bytes());
            s[at + 26..at + 28].copy_from_slice(&(v as u16).to_le_bytes());
        }
        FieldKind::ExfatChecksum => s[at + 2..at + 4].copy_from_slice(&bytes[..2]),
        FieldKind::ExfatStreamFlags => s[at + 1] = bytes[0],
        FieldKind::ExfatStreamCluster => s[at + 20..at + 24].copy_from_slice(&bytes[..4]),
        FieldKind::ExfatBitmapLen => s[at + 24..at + 32].copy_from_slice(&bytes),
    }
}

pub(crate) const DOT: &[u8; 11] = b".          ";
pub(crate) const DOT_DOT: &[u8; 11] = b"..         ";

/// The FAT32 directory now starting at `new`: its `.` and each subdirectory's `..`, `old` before.
fn dot_dot<D: Device>(dev: &mut D, geo: &Geometry, cache: &mut Cache, old: u32, new: u32, root: bool) -> Result<()> {
    let mut fields = Vec::new();
    let mut cluster = new;
    let mut walked = 0u32;
    'walk: loop {
        let lba = geo.cluster_lba(cluster);
        cache.load(dev, [(lba, geo.cluster_sectors)])?;
        for s in 0..geo.cluster_sectors {
            let sector = *cache.get(dev, lba + s)?;
            for e in 0..SECTOR / 32 {
                let entry = &sector[e * 32..][..32];
                let first_entry = walked == 0 && s == 0 && e == 0;
                match entry[0] {
                    0 => break 'walk,
                    0xe5 => continue,
                    _ => {}
                }
                let attr = entry[11];
                if attr & 0x3f == 0x0f || attr & 0x08 != 0 || &entry[..11] == DOT_DOT {
                    continue;
                }
                if &entry[..11] == DOT {
                    if first_entry && !root {
                        fields.push((lba, e * 32));
                    }
                    continue;
                }
                if attr & 0x10 != 0 {
                    let child = get_field(&sector, e * 32, FieldKind::Fat32Cluster) as u32;
                    if child < 2 || child >= geo.cluster_end {
                        return Err(changed(lba + s, "a subdirectory's first cluster"));
                    }
                    let child_lba = geo.cluster_lba(child);
                    if &cache.get(dev, child_lba)?[32..43] != DOT_DOT {
                        return Err(changed(child_lba, "a subdirectory's .. entry"));
                    }
                    fields.push((child_lba, 32));
                }
            }
        }
        walked += 1;
        let next = fat_value(dev, geo, cache, cluster)?;
        if next >= FAT32_EOC {
            break;
        }
        if next < 2 || next >= geo.cluster_end || walked >= geo.cluster_end {
            return Err(changed(lba, "a directory's chain"));
        }
        cluster = next;
    }
    for (lba, at) in fields {
        let sector = cache.get(dev, lba)?;
        let current = get_field(sector, at, FieldKind::Fat32Cluster);
        if current == u64::from(new) || (root && current == 0) {
            continue;
        }
        if current != u64::from(old) {
            return Err(changed(lba, "a . or .. entry"));
        }
        set_field(sector, at, FieldKind::Fat32Cluster, u64::from(new));
    }
    Ok(())
}
