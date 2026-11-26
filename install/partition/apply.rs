// SPDX-License-Identifier: GPL-3.0-or-later
//! Applying a journaled step to the card: each group's sectors read, every value checked to be the record's old or
//! new one, the changed sectors written in long runs, then one barrier before the next group.

use switch_card::{Device, SECTOR};

use crate::cache::Cache;
use crate::error::{Error, Result};
use crate::journal::{FieldKind, Record, Step};

const FAT32_EOC: u32 = 0x0fff_fff8;

/// Where a volume's FAT and clusters sit, for turning records into sectors.
#[derive(Clone, Debug)]
pub struct Geometry {
    pub fat_lbas: Vec<u64>,
    pub fat_mask: u32,
    pub heap_lba: u64,
    pub cluster_sectors: u64,
    /// One past the highest cluster number in use.
    pub cluster_end: u32,
}

impl Geometry {
    pub fn cluster_lba(&self, cluster: u32) -> u64 {
        self.heap_lba + u64::from(cluster - 2) * self.cluster_sectors
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
}

/// Applies every group of `step` in order; the number of write requests made.
pub fn apply<D: Device>(dev: &mut D, geo: &Geometry, step: &Step) -> Result<usize> {
    precheck_fields(dev, step)?;
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

fn precheck_fields<D: Device>(dev: &mut D, step: &Step) -> Result<()> {
    let mut cache = Cache::default();
    let ranges: Vec<(u64, u64)> = step
        .groups
        .iter()
        .flatten()
        .filter_map(|r| match *r {
            Record::Field { lba, .. } => Some((lba, 1)),
            _ => None,
        })
        .collect();
    if ranges.is_empty() {
        return Ok(());
    }
    cache.load(dev, ranges)?;
    for r in step.groups.iter().flatten() {
        if let Record::Field {
            lba,
            offset,
            kind,
            old,
            new,
        } = *r
        {
            let sector = cache.get(dev, lba)?;
            let at = offset as usize;
            if kind == FieldKind::Fat32Cluster && (sector[at] == 0x00 || sector[at] == 0xe5) {
                return Err(changed(lba, "a deleted or free directory entry"));
            }
            let current = get_field(sector, at, kind);
            if current != new && current != old {
                return Err(changed(lba, "a directory entry or boot sector field"));
            }
        }
    }
    Ok(())
}

fn prefetch(geo: &Geometry, group: &[Record]) -> Vec<(u64, u64)> {
    let mut ranges = Vec::new();
    let fat_range = |ranges: &mut Vec<(u64, u64)>, first: u32, len: u32| {
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
            _ => {}
        }
    }
    ranges
}

fn changed(lba: u64, what: &'static str) -> Error {
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
            if kind == FieldKind::Fat32Cluster && (sector[at] == 0x00 || sector[at] == 0xe5) {
                return Err(changed(lba, "a deleted or free directory entry"));
            }
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
                    return Err(changed(lba, "a partition table sector"));
                }
                *sector = **new;
            }
        }
    }
    Ok(())
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
    let (lba, at) = geo.fat_entry(0, index);
    let sector = cache.get(dev, lba)?;
    Ok(u32::from_le_bytes(sector[at..at + 4].try_into().unwrap()) & geo.fat_mask)
}

pub fn get_field(s: &[u8; SECTOR], at: usize, kind: FieldKind) -> u64 {
    let le = |n: usize| {
        let mut v = [0u8; 8];
        v[..n].copy_from_slice(&s[at..at + n]);
        u64::from_le_bytes(v)
    };
    match kind {
        FieldKind::Le8 => le(1),
        FieldKind::Le16 => le(2),
        FieldKind::Le32 => le(4),
        FieldKind::Le64 => le(8),
        FieldKind::Fat32Cluster => {
            let half = |o: usize| u64::from(u16::from_le_bytes([s[at + o], s[at + o + 1]]));
            half(20) << 16 | half(26)
        }
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
    }
}

const DOT: &[u8; 11] = b".          ";
const DOT_DOT: &[u8; 11] = b"..         ";

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
