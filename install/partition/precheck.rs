// SPDX-License-Identifier: GPL-3.0-or-later
//! Pre-write validation of every record in a journaled step before any group mutates the card.

use std::collections::HashMap;

use switch_card::{Device, SECTOR};

use crate::apply::{DOT, DOT_DOT, FAT32_EOC, Geometry, changed, check_field_liveness, fat_read, get_field, prefetch};
use crate::cache::Cache;
use crate::error::{Error, Result};
use crate::journal::{FieldKind, Record, Step};

/// Verifies every record across all groups before any write: each value must be old or new, live directory entries
/// must still be in use, and no group after an incomplete barriered group may have started.
pub fn precheck<D: Device>(dev: &mut D, geo: &Geometry, step: &Step) -> Result<()> {
    let mut cache = Cache::default();
    let mut ranges = Vec::new();
    let mut has_dot_dot = false;
    for group in &step.groups {
        ranges.extend(prefetch(geo, group));
        for r in group {
            match *r {
                Record::Fat { index, .. } if !geo.bitmap.is_empty() => {
                    geo.bitmap_ranges(&mut ranges, index, 1);
                }
                Record::DotDot { .. } => has_dot_dot = true,
                _ => {}
            }
        }
    }
    if ranges.is_empty() && !has_dot_dot {
        return Ok(());
    }
    cache.load(dev, ranges)?;

    let mut chains: Vec<(u32, u32, u32)> = Vec::new();
    let mut fat_edits: HashMap<u32, u32> = HashMap::new();
    let mut field_edits: HashMap<(u64, u16), u32> = HashMap::new();
    let mut earlier_incomplete = false;

    for group in &step.groups {
        let mut group_incomplete = false;
        for record in group {
            match *record {
                Record::Field {
                    lba,
                    offset,
                    kind,
                    old,
                    new,
                } => {
                    if !kind.valid_offset(offset) {
                        return Err(Error::Journal("field offset out of sector bounds"));
                    }
                    let sector = cache.get(dev, lba)?;
                    let at = offset as usize;
                    check_field_liveness(sector, at, kind, lba)?;
                    let current = get_field(sector, at, kind);
                    if current != new && current != old {
                        return Err(changed(lba, "a directory entry or boot sector field"));
                    }
                    if earlier_incomplete && current != old {
                        return Err(changed(lba, "a directory entry or boot sector field"));
                    }
                    if current != new {
                        group_incomplete = true;
                    }
                    if has_dot_dot && kind == FieldKind::Fat32Cluster {
                        field_edits.insert((lba, offset), new as u32);
                    }
                }
                Record::Fat { index, old, new } => {
                    check_cluster(geo, index)?;
                    if !geo.bitmap.is_empty() {
                        let (bm_lba, off, mask) = geo.bitmap_pos(index)?;
                        if cache.get(dev, bm_lba)?[off] & mask == 0 {
                            return Err(changed(bm_lba, "a cluster freed while the journal was pending"));
                        }
                    }
                    for copy in 0..geo.fat_lbas.len() {
                        let (lba, current) = fat_read(dev, geo, &mut cache, copy, index)?;
                        if current != new && current != old {
                            return Err(changed(lba, "a FAT entry"));
                        }
                        if earlier_incomplete && current != old {
                            return Err(changed(lba, "a FAT entry"));
                        }
                        if current != new {
                            group_incomplete = true;
                        }
                    }
                    if has_dot_dot {
                        fat_edits.insert(index, new);
                    }
                }
                Record::Free { first, len, last } => {
                    check_cluster_range(geo, first, len)?;
                    for i in 0..len {
                        let old = if i + 1 < len { first + i + 1 } else { last };
                        for copy in 0..geo.fat_lbas.len() {
                            let (lba, current) = fat_read(dev, geo, &mut cache, copy, first + i)?;
                            if current != 0 && current != old {
                                return Err(changed(lba, "a FAT entry"));
                            }
                            if earlier_incomplete && current != old {
                                return Err(changed(lba, "a FAT entry"));
                            }
                            if current != 0 {
                                group_incomplete = true;
                            }
                        }
                    }
                }
                Record::Chain {
                    first,
                    len,
                    last,
                    loose,
                } => {
                    check_cluster_range(geo, first, len)?;
                    for i in 0..len {
                        let new = if i + 1 < len { first + i + 1 } else { last };
                        for copy in 0..geo.fat_lbas.len() {
                            let (lba, current) = fat_read(dev, geo, &mut cache, copy, first + i)?;
                            if !loose && current != 0 && current != new {
                                return Err(changed(lba, "a FAT entry"));
                            }
                            if earlier_incomplete && !loose && current != 0 {
                                return Err(changed(lba, "a FAT entry"));
                            }
                            if current != new {
                                group_incomplete = true;
                            }
                        }
                    }
                    if has_dot_dot && len > 0 {
                        chains.push((first, len, last));
                    }
                }
                Record::Bits { first, len, set } => {
                    check_cluster_range(geo, first, len)?;
                    for i in 0..len {
                        let (lba, off, mask) = geo.bitmap_pos(first + i)?;
                        let bit = cache.get(dev, lba)?[off] & mask != 0;
                        if earlier_incomplete && bit == set {
                            return Err(changed(lba, "an allocation bitmap bit"));
                        }
                        if bit != set {
                            group_incomplete = true;
                        }
                    }
                }
                Record::Sector { lba, old, ref new } => {
                    let sector = cache.get(dev, lba)?;
                    let is_new = sector == &**new;
                    let is_old = crc32fast::hash(&sector[..]) == old;
                    if !is_new && !is_old {
                        return Err(changed(lba, "a partition table or boot sector"));
                    }
                    if earlier_incomplete && !is_old {
                        return Err(changed(lba, "a partition table or boot sector"));
                    }
                    if !is_new {
                        group_incomplete = true;
                    }
                }
                Record::DotDot { old, new, root } => {
                    if precheck_dot_dot(
                        dev,
                        geo,
                        &mut cache,
                        &chains,
                        &fat_edits,
                        &field_edits,
                        old,
                        new,
                        root,
                        earlier_incomplete,
                    )? {
                        group_incomplete = true;
                    }
                }
                Record::Zero { .. } => {}
            }
        }
        earlier_incomplete |= group_incomplete;
    }
    Ok(())
}

/// Validates a FAT32 `DotDot` record read-only, overlaying earlier groups' `Chain`, `Fat`, and `Fat32Cluster` edits
/// so moved directory chains can be walked even before Group 0 writes them to disk.
#[allow(clippy::too_many_arguments)]
fn precheck_dot_dot<D: Device>(
    dev: &mut D,
    geo: &Geometry,
    cache: &mut Cache,
    chains: &[(u32, u32, u32)],
    fat_edits: &HashMap<u32, u32>,
    field_edits: &HashMap<(u64, u16), u32>,
    old: u32,
    new: u32,
    root: bool,
    earlier_incomplete: bool,
) -> Result<bool> {
    check_cluster(geo, old)?;
    check_cluster(geo, new)?;
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
                    let child = field_edits
                        .get(&(lba + s, (e * 32) as u16))
                        .copied()
                        .unwrap_or_else(|| get_field(&sector, e * 32, FieldKind::Fat32Cluster) as u32);
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
        let next = overlay_fat(dev, geo, cache, chains, fat_edits, cluster)?;
        if next >= FAT32_EOC {
            break;
        }
        if next < 2 || next >= geo.cluster_end || walked >= geo.cluster_end {
            return Err(changed(lba, "a directory's chain"));
        }
        cluster = next;
    }

    let mut incomplete = false;
    for (lba, at) in fields {
        let sector = cache.get(dev, lba)?;
        let current = get_field(sector, at, FieldKind::Fat32Cluster);
        if current == u64::from(new) {
            if earlier_incomplete {
                return Err(changed(lba, "a . or .. entry"));
            }
        } else if root && current == 0 {
            continue;
        } else if current == u64::from(old) {
            incomplete = true;
        } else {
            return Err(changed(lba, "a . or .. entry"));
        }
    }
    Ok(incomplete)
}

fn overlay_fat<D: Device>(
    dev: &mut D,
    geo: &Geometry,
    cache: &mut Cache,
    chains: &[(u32, u32, u32)],
    fat_edits: &HashMap<u32, u32>,
    cluster: u32,
) -> Result<u32> {
    if let Some(&v) = fat_edits.get(&cluster) {
        return Ok(v);
    }
    for &(first, len, last) in chains {
        if cluster >= first && cluster < first + len {
            return Ok(if cluster + 1 < first + len { cluster + 1 } else { last });
        }
    }
    let (_, current) = fat_read(dev, geo, cache, 0, cluster)?;
    Ok(current)
}

fn check_cluster(geo: &Geometry, cluster: u32) -> Result<()> {
    if cluster < 2 || cluster >= geo.cluster_end {
        return Err(Error::Journal("cluster record out of volume bounds"));
    }
    Ok(())
}

fn check_cluster_range(geo: &Geometry, first: u32, len: u32) -> Result<()> {
    if len > 0 && (first < 2 || first.checked_add(len).is_none_or(|e| e > geo.cluster_end)) {
        return Err(Error::Journal("cluster range out of volume bounds"));
    }
    Ok(())
}
