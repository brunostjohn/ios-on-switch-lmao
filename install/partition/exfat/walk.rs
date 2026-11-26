// SPDX-License-Identifier: GPL-3.0-or-later
//! Every file and directory on an exFAT volume, found from the root, validating entry sets, checksums, NoFatChain and
//! FAT chains, bitmap ownership, and lost/cross-linked clusters.

use std::collections::VecDeque;

use switch_card::Device;

use super::{Exfat, read_clusters};
use crate::error::{Error, Result};
use crate::model::{self, Bits, Head};

pub const FLAG_ALLOC_POSSIBLE: u8 = 0x01;
pub const FLAG_NO_FAT_CHAIN: u8 = 0x02;
const ATTR_DIRECTORY: u16 = 0x0010;
const MAX_DIR_BYTES: u64 = 256 << 20;

#[derive(Clone, Debug)]
pub struct ExfatStream {
    pub first: u32,
    pub dir: bool,
    pub head: Head,
    pub no_fat_chain: bool,
    pub cluster_count: u32,
    pub flags: u8,
    pub checksum: u16,
    /// Full raw directory entry set (populated during relocation for streams reaching `cut`).
    pub entry_set: Vec<u8>,
}

pub struct ExfatTree {
    pub streams: Vec<ExfatStream>,
    pub highest: Vec<u32>,
    pub owned: Bits,
    pub lost_end: u32,
}

impl ExfatTree {
    pub fn affected(&self, cut: u32) -> Vec<u32> {
        let hit = |want_dir: bool| {
            (0..self.streams.len() as u32)
                .filter(move |&i| self.streams[i as usize].dir == want_dir && self.highest[i as usize] >= cut)
        };
        hit(true).chain(hit(false)).collect()
    }

    pub fn owned_from(&self, cut: u32, end: u32) -> u64 {
        (cut..end).filter(|&c| self.owned.get(c)).count() as u64
    }

    pub fn lowest_cut(&self, fs: &Exfat) -> u32 {
        let pinned = self.lost_end.max(fs.system_end());
        model::lowest_cut(fs.cluster_end(), |c| self.owned.get(c), |c| !fs.is_used(c), pinned)
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

pub fn walk<D: Device>(dev: &mut D, fs: &Exfat, keep_sets_from: Option<u32>) -> Result<ExfatTree> {
    let end = fs.cluster_end();
    let cb = fs.vbr.cluster_bytes();
    let geo = fs.geometry();
    let mut seen = Bits::new(end);

    let claim_clusters = |chain: &[u32], seen: &mut Bits| -> Result<u32> {
        let mut max = 0u32;
        for &c in chain {
            if !fs.is_used(c) {
                return Err(Error::Damaged(format!(
                    "cluster {c} is referenced by a stream but marked free in the allocation bitmap"
                )));
            }
            if !seen.set(c) {
                return Err(Error::Damaged(format!("cluster {c} belongs to two chains")));
            }
            max = max.max(c);
        }
        Ok(max)
    };

    claim_clusters(&fs.bitmap_chain, &mut seen)?;
    claim_clusters(&fs.upcase_chain, &mut seen)?;

    let root_chain = fs.chain(fs.vbr.root)?;
    let root_max = claim_clusters(&root_chain, &mut seen)?;
    let root = ExfatStream {
        first: fs.vbr.root,
        dir: true,
        head: Head::Root,
        no_fat_chain: false,
        cluster_count: root_chain.len() as u32,
        flags: 0,
        checksum: 0,
        entry_set: Vec::new(),
    };
    let mut streams = vec![root];
    let mut highest = vec![root_max];
    let mut queue: VecDeque<(u32, Vec<u32>)> = VecDeque::from([(0u32, root_chain)]);

    while let Some((id, chain)) = queue.pop_front() {
        let is_root = id == 0;
        let mut saw_label = false;
        let mut saw_guid = false;
        let data = read_clusters(dev, &geo, &chain)?;
        let total = data.len() / 32;
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
                0xa1 => {
                    return Err(Error::Damaged("TexFAT padding entry is not supported".into()));
                }
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
            if entry_set_checksum(set) != stored_sum {
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

            let offset = i as u64 * 32;
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
            if expected_clusters > u64::from(fs.vbr.cluster_count) {
                return Err(Error::Damaged("stream DataLength exceeds volume size".into()));
            }
            let child_chain = fs.stream_clusters(first, expected_clusters as u32, no_fat_chain)?;
            if child_chain.len() as u64 != expected_clusters {
                return Err(Error::Damaged(format!(
                    "stream at cluster {first} has {} clusters for {data_len} bytes",
                    child_chain.len()
                )));
            }
            let max = claim_clusters(&child_chain, &mut seen)?;
            let entry_set = if keep_sets_from.is_some_and(|cut| max >= cut) {
                set.to_vec()
            } else {
                Vec::new()
            };
            let child_id = streams.len() as u32;
            streams.push(ExfatStream {
                first,
                dir: is_dir,
                head: Head::Entry { parent: id, offset },
                no_fat_chain,
                cluster_count: expected_clusters as u32,
                flags,
                checksum: stored_sum,
                entry_set,
            });
            highest.push(max);
            if is_dir {
                queue.push_back((child_id, child_chain));
            }
        }
    }

    let lost_end = (2..end)
        .rev()
        .find(|&c| fs.is_used(c) && !seen.get(c))
        .map_or(2, |c| c + 1);
    Ok(ExfatTree {
        streams,
        highest,
        owned: seen,
        lost_end,
    })
}
