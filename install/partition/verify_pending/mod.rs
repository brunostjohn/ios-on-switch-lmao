// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only verification of a `Pending` journaled step before `resume()` applies it, blocking any intervening card
//! use (reallocated destinations/sources, deleted/reused chains, overwritten cluster data, or files allocated past
//! the shrink boundary) before a single sector is written.

mod readers;
mod walk;

use std::collections::{HashMap, HashSet};

use switch_card::{Device, SECTOR};

use crate::apply::{DOT, DOT_DOT, Geometry, changed, get_field, set_field};
use crate::cache::{Cache, MAX_REQUEST};
use crate::error::{Error, Result};
use crate::fat32::boot::ROOT_CLUS;
use crate::journal::{FieldKind, FsKind, Phase, Plan, Record, Step};
use readers::LiveMap;
use walk::{walk_exfat_tolerant, walk_fat32_tolerant};

pub fn verify_pending<D: Device>(dev: &mut D, plan: &Plan, phase: Phase, geo: &Geometry, step: &Step) -> Result<()> {
    if phase == Phase::Tables || step.groups.is_empty() {
        return Ok(());
    }

    let mut live = match plan.fs {
        FsKind::Exfat => {
            let field_targets = collect_exfat_field_targets(step);
            walk_exfat_tolerant(dev, plan, geo, &field_targets)?
        }
        FsKind::Fat32 => walk_fat32_tolerant(dev, plan, geo)?,
    };

    match phase {
        Phase::Relocate => {
            let is_move = step
                .groups
                .last()
                .is_some_and(|g| g.iter().any(|r| matches!(r, Record::Free { .. })));
            if is_move {
                verify_move(dev, plan, geo, step, &mut live)?;
            } else {
                verify_convert(geo, step, &live)?;
            }
        }
        Phase::Shrink => verify_shrink(dev, geo, step, &mut live)?,
        Phase::Tables => unreachable!(),
    }
    Ok(())
}

fn collect_exfat_field_targets(step: &Step) -> HashSet<(u64, u16)> {
    let mut targets = HashSet::new();
    for group in &step.groups {
        for r in group {
            if let Record::Field { lba, offset, kind, .. } = *r
                && matches!(
                    kind,
                    FieldKind::ExfatChecksum | FieldKind::ExfatStreamFlags | FieldKind::ExfatStreamCluster
                )
            {
                targets.insert((lba, offset));
            }
        }
    }
    targets
}

fn verify_move<D: Device>(dev: &mut D, plan: &Plan, geo: &Geometry, step: &Step, live: &mut LiveMap) -> Result<()> {
    if step.groups.len() < 3 {
        return Err(Error::Journal("move step has fewer than 3 groups"));
    }
    let chains: Vec<(u32, u32)> = step.groups[0]
        .iter()
        .filter_map(|r| match *r {
            Record::Chain { first, len, .. } => Some((first, len)),
            _ => None,
        })
        .collect();
    let frees: Vec<(u32, u32)> = step
        .groups
        .last()
        .unwrap()
        .iter()
        .filter_map(|r| match *r {
            Record::Free { first, len, .. } => Some((first, len)),
            _ => None,
        })
        .collect();
    if chains.len() != frees.len() || chains.iter().zip(&frees).any(|(c, f)| c.1 != f.1) {
        return Err(Error::Journal("move step Chain and Free runs disagree"));
    }

    let runs: Vec<(u32, u32, u32)> = frees
        .iter()
        .zip(&chains)
        .map(|(&(src, len), &(dst, _))| (src, dst, len))
        .collect();

    let mut un_repointed = Vec::new();
    for &(src, dst, len) in &runs {
        if len == 0 {
            continue;
        }
        let src_owned = live.owned.get(src);
        let dst_owned = live.owned.get(dst);
        if src_owned == dst_owned {
            return Err(changed(
                geo.cluster_lba(if src_owned { dst } else { src }),
                "moved cluster ownership disagrees with the journal",
            ));
        }
        for i in 0..len {
            let s = live.owned.get(src + i);
            let d = live.owned.get(dst + i);
            if s != src_owned || d != dst_owned {
                return Err(changed(geo.cluster_lba(dst + i), "moved run has split ownership"));
            }
            let active = if s { src + i } else { dst + i };
            if !live.is_fat_chained(active) {
                return Err(changed(
                    geo.cluster_lba(active),
                    "moved cluster is no longer FAT-chained",
                ));
            }
        }
        if src_owned {
            un_repointed.push((src, dst, len));
        }
    }

    let repoint = &step.groups[step.groups.len() - 2];
    let mut cache = Cache::default();
    let mut field_lbas = Vec::new();
    for r in repoint {
        if let Record::Field { lba, kind, .. } = *r
            && matches!(
                kind,
                FieldKind::ExfatStreamCluster | FieldKind::Fat32Cluster | FieldKind::Le32
            )
        {
            field_lbas.push((lba, 1));
        }
    }
    cache.load(dev, field_lbas)?;

    for r in repoint {
        match *r {
            Record::Fat { index: pred, old, new } => {
                check_field_cluster(geo, u64::from(old))?;
                check_field_cluster(geo, u64::from(new))?;
                if !live.owned.get(pred) || !live.is_fat_chained(pred) {
                    return Err(changed(
                        geo.cluster_lba(pred),
                        "repointed FAT predecessor is no longer live and FAT-chained",
                    ));
                }
                let cur = live.fat.get(dev, pred)?;
                let (want_live, want_dead) = if cur == old {
                    (old, new)
                } else if cur == new {
                    (new, old)
                } else {
                    return Err(changed(geo.cluster_lba(pred), "a FAT entry"));
                };
                if !live.owned.get(want_live) || live.owned.get(want_dead) {
                    return Err(changed(
                        geo.cluster_lba(want_live),
                        "repointed FAT target ownership disagrees with the journal",
                    ));
                }
            }
            Record::Field {
                lba,
                offset,
                kind,
                old,
                new,
            } => match kind {
                FieldKind::ExfatStreamCluster | FieldKind::Fat32Cluster => {
                    let old_c = check_field_cluster(geo, old)?;
                    let new_c = check_field_cluster(geo, new)?;
                    let dir_cluster = geo
                        .cluster_of_lba(lba)
                        .ok_or(Error::Journal("directory field record outside cluster heap"))?;
                    // A moved parent directory's destination sector may already be updated by a partial repoint
                    // epoch while the live tree still walks the source copy (which still holds `old`).
                    let (want_live, want_dead) = if live.owned.get(dir_cluster) {
                        let cur = get_field(cache.get(dev, lba)?, offset as usize, kind);
                        if cur == old {
                            (old_c, new_c)
                        } else if cur == new {
                            (new_c, old_c)
                        } else {
                            return Err(changed(lba, "a directory entry or boot sector field"));
                        }
                    } else if is_dst_cluster(&runs, dir_cluster) {
                        (old_c, new_c)
                    } else {
                        return Err(changed(lba, "parent directory cluster is no longer live"));
                    };
                    if !live.owned.get(want_live) || live.owned.get(want_dead) {
                        return Err(changed(lba, "repointed cluster ownership disagrees with the journal"));
                    }
                }
                FieldKind::Le32 if lba == plan.fat_first && offset == ROOT_CLUS => {
                    let old_c = check_field_cluster(geo, old)?;
                    let new_c = check_field_cluster(geo, new)?;
                    let cur = get_field(cache.get(dev, lba)?, offset as usize, kind);
                    let (want_live, want_dead) = if cur == old {
                        (old_c, new_c)
                    } else if cur == new {
                        (new_c, old_c)
                    } else {
                        return Err(changed(lba, "a directory entry or boot sector field"));
                    };
                    if !live.owned.get(want_live) || live.owned.get(want_dead) {
                        return Err(changed(lba, "root cluster ownership disagrees with the journal"));
                    }
                }
                _ => {}
            },
            _ => {}
        }
    }

    verify_run_data(dev, geo, repoint, &un_repointed)
}

fn verify_run_data<D: Device>(
    dev: &mut D,
    geo: &Geometry,
    repoint: &[Record],
    un_repointed: &[(u32, u32, u32)],
) -> Result<()> {
    if un_repointed.is_empty() {
        return Ok(());
    }
    let mut field_edits: HashMap<u64, Vec<(u16, FieldKind, u64, u64)>> = HashMap::new();
    let mut dot_by_lba: HashMap<u64, (u32, u32)> = HashMap::new();
    let mut dotdots: Vec<(u32, u32)> = Vec::new();
    for r in repoint {
        match *r {
            Record::Field {
                lba,
                offset,
                kind,
                old,
                new,
            } => {
                field_edits.entry(lba).or_default().push((offset, kind, old, new));
            }
            Record::DotDot { old, new, root } => {
                if !root {
                    dot_by_lba.insert(geo.cluster_lba(new), (old, new));
                }
                dotdots.push((old, new));
            }
            _ => {}
        }
    }

    let buf_sectors = un_repointed
        .iter()
        .map(|&(_, _, len)| u64::from(len) * geo.cluster_sectors)
        .max()
        .unwrap_or(0)
        .min(MAX_REQUEST) as usize;
    let mut src_buf = vec![0u8; buf_sectors * SECTOR];
    let mut dst_buf = vec![0u8; buf_sectors * SECTOR];

    for &(src, dst, len) in un_repointed {
        let src_base = geo.cluster_lba(src);
        let dst_base = geo.cluster_lba(dst);
        let total = u64::from(len) * geo.cluster_sectors;
        let mut done = 0u64;
        while done < total {
            let n = (total - done).min(MAX_REQUEST);
            let bytes = n as usize * SECTOR;
            dev.read_sectors(src_base + done, &mut src_buf[..bytes])?;
            dev.read_sectors(dst_base + done, &mut dst_buf[..bytes])?;
            for s in 0..n {
                let off = s as usize * SECTOR;
                let src_sec: &[u8; SECTOR] = src_buf[off..off + SECTOR].try_into().unwrap();
                let dst_sec: &[u8; SECTOR] = dst_buf[off..off + SECTOR].try_into().unwrap();
                if src_sec == dst_sec {
                    continue;
                }
                let dst_lba = dst_base + done + s;
                let mut norm_dst = *dst_sec;
                if let Some(edits) = field_edits.get(&dst_lba) {
                    for &(f_off, kind, old, new) in edits {
                        if get_field(&norm_dst, f_off as usize, kind) == new {
                            set_field(&mut norm_dst, f_off as usize, kind, old);
                        }
                    }
                }
                if !dotdots.is_empty() && (done + s).is_multiple_of(geo.cluster_sectors) {
                    if let Some(&(old, new)) = dot_by_lba.get(&dst_lba)
                        && &norm_dst[..11] == DOT
                        && get_field(&norm_dst, 0, FieldKind::Fat32Cluster) == u64::from(new)
                    {
                        set_field(&mut norm_dst, 0, FieldKind::Fat32Cluster, u64::from(old));
                    }
                    if &norm_dst[32..43] == DOT_DOT && &src_sec[32..43] == DOT_DOT {
                        let d_val = get_field(&norm_dst, 32, FieldKind::Fat32Cluster);
                        let s_val = get_field(src_sec, 32, FieldKind::Fat32Cluster);
                        for &(old, new) in &dotdots {
                            if d_val == u64::from(new) && s_val == u64::from(old) {
                                set_field(&mut norm_dst, 32, FieldKind::Fat32Cluster, u64::from(old));
                                break;
                            }
                        }
                    }
                }
                if src_sec != &norm_dst {
                    return Err(changed(dst_lba, "moved cluster data"));
                }
            }
            done += n;
        }
    }
    Ok(())
}

fn verify_convert(geo: &Geometry, step: &Step, live: &LiveMap) -> Result<()> {
    for r in &step.groups[0] {
        if let Record::Chain { first, len, .. } = *r {
            for i in 0..len {
                if !live.owned.get(first + i) {
                    return Err(changed(
                        geo.cluster_lba(first + i),
                        "converted stream cluster is no longer live",
                    ));
                }
            }
        }
    }
    if let Some(fields) = step.groups.get(1) {
        for r in fields {
            if let Record::Field { lba, .. } = *r {
                let dir_cluster = geo
                    .cluster_of_lba(lba)
                    .ok_or(Error::Journal("directory field record outside cluster heap"))?;
                if !live.owned.get(dir_cluster) {
                    return Err(changed(lba, "parent directory cluster is no longer live"));
                }
            }
        }
    }
    Ok(())
}

fn verify_shrink<D: Device>(dev: &mut D, geo: &Geometry, step: &Step, live: &mut LiveMap) -> Result<()> {
    for c in live.new_cut..geo.cluster_end {
        if live.owned.get(c) {
            return Err(changed(
                geo.cluster_lba(c),
                "live cluster allocated past the shrink boundary",
            ));
        }
    }

    let mut terminated = false;
    for group in &step.groups {
        for r in group {
            if let Record::Fat {
                index: last_kept, new, ..
            } = *r
            {
                if !live.owned.get(last_kept) || !live.is_fat_chained(last_kept) {
                    return Err(changed(
                        geo.cluster_lba(last_kept),
                        "allocation bitmap chain changed while shrink was pending",
                    ));
                }
                if live.fat.get(dev, last_kept)? == new {
                    terminated = true;
                }
            }
        }
    }
    if terminated {
        for group in &step.groups {
            for r in group {
                match *r {
                    Record::Free { first, len, .. } | Record::Bits { first, len, set: false } => {
                        for i in 0..len {
                            if live.owned.get(first + i) {
                                return Err(changed(
                                    geo.cluster_lba(first + i),
                                    "surplus bitmap cluster is now owned by a live stream",
                                ));
                            }
                        }
                    }
                    _ => {}
                }
            }
        }
    }
    Ok(())
}

fn is_dst_cluster(runs: &[(u32, u32, u32)], cluster: u32) -> bool {
    runs.iter().any(|&(_, dst, len)| cluster >= dst && cluster < dst + len)
}

fn check_field_cluster(geo: &Geometry, value: u64) -> Result<u32> {
    if value < 2 || value >= u64::from(geo.cluster_end) {
        return Err(Error::Journal("field cluster record out of volume bounds"));
    }
    Ok(value as u32)
}
