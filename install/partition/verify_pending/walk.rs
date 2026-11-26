// SPDX-License-Identifier: GPL-3.0-or-later
//! Tolerant read-only live-tree walkers for `verify_pending`, tolerating transient mid-step orphans,
//! mid-shrink boot/bitmap transitions, and sector/cluster-straddling `SetChecksum` mismatches.

use std::collections::{HashSet, VecDeque};

use switch_card::{Device, SECTOR};

use super::readers::{BitmapReader, FatReader, LiveMap};
use crate::apply::{Geometry, changed};
use crate::error::{Error, Result};
use crate::exfat::boot::Vbr;
use crate::exfat::walk::{FLAG_ALLOC_POSSIBLE, FLAG_NO_FAT_CHAIN, entry_set_checksum};
use crate::exfat::{self, EOC as EXFAT_EOC};
use crate::fat32::MASK as FAT32_MASK;
use crate::fat32::boot::Bpb;
use crate::fat32::walk::{DOT, DOT_DOT, cluster_of};
use crate::journal::Plan;
use crate::model::Bits;

const ATTR_DIRECTORY: u16 = 0x0010;
const MAX_DIR_BYTES: u64 = 256 << 20;
const MAX_UPCASE_BYTES: u64 = 131_072;

pub(super) fn walk_exfat_tolerant<D: Device>(
    dev: &mut D,
    plan: &Plan,
    geo: &Geometry,
    field_targets: &HashSet<(u64, u16)>,
) -> Result<LiveMap> {
    let mut boot = [0u8; SECTOR];
    dev.read_sectors(plan.fat_first, &mut boot)?;
    let vbr = Vbr::parse(&boot, plan.fat_sectors)?;
    let end = vbr.cluster_count + 2;
    let cb = vbr.cluster_bytes();
    let new_cut = (vbr.clusters_in(vbr.shrunk_total(plan.fat_new_sectors)) + 2).min(u64::from(end)) as u32;

    let mut fat = FatReader::new(geo.fat_lbas[0], u64::from(end), u32::MAX);
    if fat.get(dev, 0)? != 0xffff_fff8 || fat.get(dev, 1)? != EXFAT_EOC {
        return Err(Error::Damaged("exFAT FAT[0..2] header entries are invalid".into()));
    }

    let root_chain = fat.follow_exfat(dev, vbr.root, end)?;
    let root_data = exfat::read_clusters(dev, geo, &root_chain)?;

    let mut bm_entry: Option<(u32, u64)> = None;
    let mut up_entry: Option<(u32, u64)> = None;
    for e in root_data.as_chunks::<32>().0 {
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
                    u32::from_le_bytes(e[20..24].try_into().unwrap()),
                    u64::from_le_bytes(e[24..32].try_into().unwrap()),
                ));
            }
            0x82 => {
                if up_entry.is_some() {
                    return Err(Error::Damaged("duplicate up-case table entry".into()));
                }
                up_entry = Some((
                    u32::from_le_bytes(e[20..24].try_into().unwrap()),
                    u64::from_le_bytes(e[24..32].try_into().unwrap()),
                ));
            }
            _ => {}
        }
    }

    let (bm_first, bm_len) = bm_entry.ok_or_else(|| Error::Damaged("missing allocation bitmap entry".into()))?;
    let min_bm_len = u64::from(vbr.cluster_count).div_ceil(8);
    if bm_len < min_bm_len {
        return Err(Error::Damaged("allocation bitmap is shorter than ClusterCount".into()));
    }
    let bitmap_chain = fat.follow_exfat(dev, bm_first, end)?;
    if (bitmap_chain.len() as u64) < bm_len.div_ceil(cb) {
        return Err(Error::Damaged("allocation bitmap chain shorter than DataLength".into()));
    }

    let (up_first, up_len) = up_entry.ok_or_else(|| Error::Damaged("missing up-case table entry".into()))?;
    if up_len == 0 || up_len > MAX_UPCASE_BYTES || !up_len.is_multiple_of(2) {
        return Err(Error::Damaged("invalid up-case table length".into()));
    }
    let upcase_chain = fat.follow_exfat(dev, up_first, end)?;
    if upcase_chain.len() as u64 != up_len.div_ceil(cb) {
        return Err(Error::Damaged(
            "up-case table chain length disagrees with DataLength".into(),
        ));
    }

    let mut bitmap = BitmapReader::new(geo, &bitmap_chain);
    let mut owned = Bits::new(end);
    let mut fat_chained = Bits::new(end);
    let mut claim = |dev: &mut D, chain: &[u32], chained: bool| -> Result<()> {
        for &c in chain {
            let (bm_lba, used) = bitmap.get(dev, c)?;
            if !used {
                return Err(changed(
                    bm_lba,
                    "a live cluster is marked free in the allocation bitmap",
                ));
            }
            if !owned.set(c) {
                return Err(changed(geo.cluster_lba(c), "a cluster belongs to two live streams"));
            }
            if chained {
                fat_chained.set(c);
            }
        }
        Ok(())
    };

    claim(dev, &bitmap_chain, true)?;
    claim(dev, &upcase_chain, true)?;
    claim(dev, &root_chain, true)?;

    let mut queue: VecDeque<(bool, Vec<u32>, Option<Vec<u8>>)> = VecDeque::from([(true, root_chain, Some(root_data))]);
    while let Some((is_root, chain, preloaded)) = queue.pop_front() {
        let data = match preloaded {
            Some(d) => d,
            None => exfat::read_clusters(dev, geo, &chain)?,
        };
        let slot_loc = |slot: usize| -> (u64, u16) {
            let byte = slot as u64 * 32;
            let cluster = chain[(byte / cb) as usize];
            let within = byte % cb;
            (
                geo.cluster_lba(cluster) + within / SECTOR as u64,
                (within % SECTOR as u64) as u16,
            )
        };
        let total = data.len() / 32;
        let mut saw_label = false;
        let mut saw_guid = false;
        let mut i = 0usize;
        while i < total {
            let e = &data[i * 32..(i + 1) * 32];
            let kind = e[0];
            match kind {
                0x00 => break,
                0x01..=0x7f => {
                    i += 1;
                    continue;
                }
                0x81 | 0x82 => {
                    if !is_root {
                        return Err(Error::Damaged(format!(
                            "root-only entry {kind:#x} found in a subdirectory"
                        )));
                    }
                    i += 1;
                    continue;
                }
                0x83 => {
                    if !is_root || e[1] > 11 || saw_label {
                        return Err(Error::Damaged("invalid or duplicate volume label entry".into()));
                    }
                    saw_label = true;
                    i += 1;
                    continue;
                }
                0xa0 => {
                    if !is_root || saw_guid || e[1] != 0 {
                        return Err(Error::Damaged("invalid or duplicate volume GUID entry".into()));
                    }
                    let stored = u16::from_le_bytes([e[2], e[3]]);
                    if entry_set_checksum(e) != stored {
                        return Err(Error::Damaged("volume GUID entry checksum mismatch".into()));
                    }
                    saw_guid = true;
                    i += 1;
                    continue;
                }
                0xa1 => return Err(Error::Damaged("TexFAT padding entry is not supported".into())),
                0x85 => {}
                _ => {
                    return Err(Error::Damaged(format!(
                        "unsupported or orphaned exFAT directory entry type {kind:#x}"
                    )));
                }
            }

            let secondary = e[1] as usize;
            if secondary < 2 || i + 1 + secondary > total {
                return Err(Error::Damaged("truncated or malformed file entry set".into()));
            }
            let set = &data[i * 32..(i + 1 + secondary) * 32];
            let stored_sum = u16::from_le_bytes([set[2], set[3]]);
            if entry_set_checksum(set) != stored_sum
                && !field_targets.contains(&slot_loc(i))
                && !field_targets.contains(&slot_loc(i + 1))
            {
                return Err(Error::Damaged("directory entry set checksum mismatch".into()));
            }
            let attrs = u16::from_le_bytes([set[4], set[5]]);
            let is_dir = attrs & ATTR_DIRECTORY != 0;

            let stream = &set[32..64];
            if stream[0] != 0xc0 {
                return Err(Error::Damaged("file entry not followed by stream extension".into()));
            }
            let flags = stream[1];
            if flags & FLAG_ALLOC_POSSIBLE == 0 || flags & !(FLAG_ALLOC_POSSIBLE | FLAG_NO_FAT_CHAIN) != 0 {
                return Err(Error::Damaged(format!("unsupported stream extension flags {flags:#x}")));
            }
            let no_fat_chain = flags & FLAG_NO_FAT_CHAIN != 0;
            let name_len = stream[3] as usize;
            let name_entries = name_len.div_ceil(15);
            if name_len == 0 || 1 + name_entries > secondary {
                return Err(Error::Damaged("invalid file name length in stream extension".into()));
            }
            for k in 0..name_entries {
                let ne = &set[(2 + k) * 32..(3 + k) * 32];
                if ne[0] != 0xc1 || ne[1] != 0 {
                    return Err(Error::Damaged("malformed file name directory entry".into()));
                }
            }
            for k in (1 + name_entries)..secondary {
                let extra = &set[(1 + k) * 32..(2 + k) * 32];
                if extra[0] < 0xe0 {
                    return Err(Error::Damaged(format!(
                        "unsupported critical secondary entry {:#x}",
                        extra[0]
                    )));
                }
                if extra[1] & FLAG_ALLOC_POSSIBLE != 0 {
                    return Err(Error::Volume(
                        "vendor allocation directory entries are not supported".into(),
                    ));
                }
            }

            let valid_len = u64::from_le_bytes(stream[8..16].try_into().unwrap());
            let first = u32::from_le_bytes(stream[20..24].try_into().unwrap());
            let data_len = u64::from_le_bytes(stream[24..32].try_into().unwrap());
            if valid_len > data_len {
                return Err(Error::Damaged("stream ValidDataLength exceeds DataLength".into()));
            }
            i += 1 + secondary;

            if is_dir {
                if first < 2 || data_len == 0 || data_len > MAX_DIR_BYTES || !data_len.is_multiple_of(cb) {
                    return Err(Error::Damaged("directory has invalid cluster or size".into()));
                }
                if valid_len != data_len {
                    return Err(Error::Damaged(
                        "directory ValidDataLength disagrees with DataLength".into(),
                    ));
                }
            } else if data_len == 0 {
                if first != 0 {
                    return Err(Error::Damaged("zero-length file has a non-zero first cluster".into()));
                }
                continue;
            } else if first < 2 {
                return Err(Error::Damaged("non-empty file has no first cluster".into()));
            }

            let expected_clusters = data_len.div_ceil(cb);
            if expected_clusters > u64::from(vbr.cluster_count) {
                return Err(Error::Damaged("stream DataLength exceeds volume size".into()));
            }
            let child_chain = if no_fat_chain {
                let chain_end = first
                    .checked_add(expected_clusters as u32)
                    .filter(|&e| first >= 2 && e <= end)
                    .ok_or_else(|| {
                        Error::Damaged(format!("contiguous stream from cluster {first} leaves the volume"))
                    })?;
                (first..chain_end).collect()
            } else {
                let c = fat.follow_exfat(dev, first, end)?;
                if c.len() as u64 != expected_clusters {
                    return Err(Error::Damaged(format!(
                        "stream at cluster {first} has {} clusters for {data_len} bytes",
                        c.len()
                    )));
                }
                c
            };
            claim(dev, &child_chain, !no_fat_chain)?;
            if is_dir {
                queue.push_back((false, child_chain, None));
            }
        }
    }

    Ok(LiveMap {
        owned,
        fat_chained: Some(fat_chained),
        fat,
        new_cut,
    })
}

pub(super) fn walk_fat32_tolerant<D: Device>(dev: &mut D, plan: &Plan, geo: &Geometry) -> Result<LiveMap> {
    let mut boot = [0u8; SECTOR];
    dev.read_sectors(plan.fat_first, &mut boot)?;
    let bpb = Bpb::parse(&boot, plan.fat_sectors)?;
    let end = bpb.clusters() as u32 + 2;
    let cb = geo.cluster_bytes();
    let new_cut = (bpb.clusters_in(bpb.shrunk_total(plan.fat_new_sectors)) + 2).min(u64::from(end)) as u32;

    let mut fat = FatReader::new(geo.fat_lbas[0], u64::from(end), FAT32_MASK);
    let fat1 = fat.get(dev, 1)?;
    if fat1 & 0x0800_0000 == 0 || fat1 & 0x0400_0000 == 0 {
        return Err(Error::Damaged(
            "the volume was not cleanly unmounted or had I/O errors".into(),
        ));
    }

    let mut owned = Bits::new(end);
    let mut claim = |chain: &[u32]| -> Result<()> {
        for &c in chain {
            if !owned.set(c) {
                return Err(changed(geo.cluster_lba(c), "a cluster belongs to two live streams"));
            }
        }
        Ok(())
    };

    let root_chain = fat.follow_fat32(dev, bpb.root, end)?;
    claim(&root_chain)?;
    let mut queue = VecDeque::from([root_chain]);

    while let Some(chain) = queue.pop_front() {
        let data = exfat::read_clusters(dev, geo, &chain)?;
        for entry in data.as_chunks::<32>().0 {
            match entry[0] {
                0 => break,
                0xe5 => continue,
                _ => {}
            }
            let attr = entry[11];
            if attr & 0x3f == 0x0f || attr & 0x08 != 0 || &entry[..11] == DOT || &entry[..11] == DOT_DOT {
                continue;
            }
            let first = cluster_of(entry);
            let is_dir = attr & 0x10 != 0;
            let file_size = u64::from(u32::from_le_bytes(entry[28..32].try_into().unwrap()));
            if is_dir {
                if first == 0 {
                    return Err(Error::Damaged("a directory with no clusters".into()));
                }
                if file_size != 0 {
                    return Err(Error::Damaged("a directory has a non-zero file size".into()));
                }
            } else if first == 0 {
                if file_size != 0 {
                    return Err(Error::Damaged("a non-empty file has no clusters".into()));
                }
                continue;
            }
            let child_chain = fat.follow_fat32(dev, first, end)?;
            claim(&child_chain)?;
            if is_dir {
                queue.push_back(child_chain);
            } else if child_chain.len() as u64 != file_size.div_ceil(cb) {
                return Err(Error::Damaged(format!(
                    "file at cluster {first} has {} clusters for {file_size} bytes",
                    child_chain.len()
                )));
            }
        }
    }

    Ok(LiveMap {
        owned,
        fat_chained: None,
        fat,
        new_cut,
    })
}
