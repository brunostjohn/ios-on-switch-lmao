// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT relocation and volume shrink steps: first converting affected `NoFatChain` streams to explicit FAT chains,
//! then moving clusters out of the cut in batches, then shrinking the boot regions and allocation bitmap.

use std::collections::HashMap;

use switch_card::SECTOR;

use super::walk::{ExfatStream, ExfatTree, FLAG_NO_FAT_CHAIN, entry_set_checksum};
use super::{EOC, Exfat};
use crate::apply::Geometry;
use crate::error::{Error, Result};
use crate::journal::{self, FieldKind, Record, Step};
use crate::model::Head;

const BATCH_BYTES: u64 = 1 << 30;
const BUDGET: usize = journal::BODY_BYTES - 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Run {
    src: u32,
    dst: u32,
    len: u32,
}

enum Pred {
    Fat { index: u32 },
    Head { stream: u32 },
}

pub struct ConvertBatch {
    pub step: Step,
    converted: Vec<(u32, u8, u16)>,
}

pub struct MoveBatch {
    pub step: Step,
    pub copies: Vec<(u64, u64, u64)>,
    heads: Vec<(u32, u32, u16)>,
    fat: Vec<(u32, u32)>,
    alloc_runs: Vec<(u32, u32)>,
    free_runs: Vec<(u32, u32)>,
}

struct Allocator {
    extents: Vec<(u32, u32)>,
    at: usize,
}

impl Allocator {
    fn below(fs: &Exfat, cut: u32) -> Allocator {
        let mut extents = Vec::new();
        let mut c = 2;
        while c < cut {
            if fs.is_used(c) {
                c += 1;
                continue;
            }
            let start = c;
            while c < cut && !fs.is_used(c) {
                c += 1;
            }
            extents.push((start, c - start));
        }
        extents.sort_by_key(|&(start, len)| (std::cmp::Reverse(len), start));
        Allocator { extents, at: 0 }
    }

    fn take(&mut self, want: u32) -> Option<(u32, u32)> {
        while self.at < self.extents.len() && self.extents[self.at].1 == 0 {
            self.at += 1;
        }
        let (start, len) = self.extents.get_mut(self.at)?;
        let n = want.min(*len);
        let got = (*start, n);
        *start += n;
        *len -= n;
        Some(got)
    }
}

pub(super) fn entry_loc(
    geo: &Geometry,
    parent_chain: &[u32],
    moved: &HashMap<u32, u32>,
    byte_offset: u64,
) -> (u64, u16) {
    let cb = geo.cluster_bytes();
    let here = parent_chain[(byte_offset / cb) as usize];
    let cluster = moved.get(&here).copied().unwrap_or(here);
    let within = byte_offset % cb;
    (
        geo.cluster_lba(cluster) + within / SECTOR as u64,
        (within % SECTOR as u64) as u16,
    )
}

fn parent_chain_for(
    fs: &Exfat,
    tree: &ExfatTree,
    chains: &mut HashMap<u32, Vec<u32>>,
    parent: u32,
) -> Result<Vec<u32>> {
    if let Some(c) = chains.get(&parent) {
        return Ok(c.clone());
    }
    let p = &tree.streams[parent as usize];
    let c = fs.stream_clusters(p.first, p.cluster_count, p.no_fat_chain)?;
    chains.insert(parent, c.clone());
    Ok(c)
}

/// Converts affected `NoFatChain` streams into explicit FAT chains before any cluster moves.
pub fn plan_convert(
    fs: &Exfat,
    tree: &ExfatTree,
    affected: &[u32],
    cursor: &mut usize,
) -> Result<Option<ConvertBatch>> {
    let geo = fs.geometry();
    let no_moves = HashMap::new();
    let mut chains = HashMap::new();
    let mut fat_group = Vec::new();
    let mut field_group = Vec::new();
    let mut converted = Vec::new();
    let mut used = 0usize;
    let per_stream_cost = 14 + 28 + 28;

    while *cursor < affected.len() {
        let id = affected[*cursor];
        let s = &tree.streams[id as usize];
        if !s.no_fat_chain {
            *cursor += 1;
            continue;
        }
        if used + per_stream_cost > BUDGET {
            break;
        }
        let Head::Entry { parent, offset } = s.head else {
            unreachable!("exFAT root directory never has NoFatChain");
        };
        let parent_chain = parent_chain_for(fs, tree, &mut chains, parent)?;
        let file_loc = entry_loc(&geo, &parent_chain, &no_moves, offset);
        let stream_loc = entry_loc(&geo, &parent_chain, &no_moves, offset + 32);
        let new_flags = s.flags & !FLAG_NO_FAT_CHAIN;
        let new_sum = recompute_checksum(s, new_flags, s.first);

        fat_group.push(Record::Chain {
            first: s.first,
            len: s.cluster_count,
            last: EOC,
            loose: true,
        });
        field_group.push(Record::Field {
            lba: stream_loc.0,
            offset: stream_loc.1,
            kind: FieldKind::ExfatStreamFlags,
            old: u64::from(s.flags),
            new: u64::from(new_flags),
        });
        field_group.push(Record::Field {
            lba: file_loc.0,
            offset: file_loc.1,
            kind: FieldKind::ExfatChecksum,
            old: u64::from(s.checksum),
            new: u64::from(new_sum),
        });
        converted.push((id, new_flags, new_sum));
        used += per_stream_cost;
        *cursor += 1;
    }

    if converted.is_empty() {
        return Ok(None);
    }
    let step = Step {
        groups: vec![fat_group, field_group],
    };
    debug_assert!(journal::fits(&step));
    Ok(Some(ConvertBatch { step, converted }))
}

impl ConvertBatch {
    pub fn apply_to(&self, fs: &mut Exfat, tree: &mut ExfatTree) {
        for &(id, new_flags, new_sum) in &self.converted {
            let s = &mut tree.streams[id as usize];
            for i in 0..s.cluster_count {
                let next = if i + 1 < s.cluster_count { s.first + i + 1 } else { EOC };
                fs.fat[(s.first + i) as usize] = next;
            }
            s.no_fat_chain = false;
            s.flags = new_flags;
            s.checksum = new_sum;
            s.entry_set[33] = new_flags;
            s.entry_set[2..4].copy_from_slice(&new_sum.to_le_bytes());
        }
    }
}

/// Plans one batch of cluster moves out of `cut..` once every affected stream has an explicit FAT chain.
pub fn plan_move(
    fs: &Exfat,
    tree: &ExfatTree,
    affected: &[u32],
    cut: u32,
    cursor: &mut usize,
) -> Result<Option<MoveBatch>> {
    let mut alloc = Allocator::below(fs, cut);
    let max_clusters = (BATCH_BYTES / fs.vbr.cluster_bytes()).max(1);
    let mut runs: Vec<Run> = Vec::new();
    let mut preds: Vec<(Pred, Run)> = Vec::new();
    let mut moved: HashMap<u32, u32> = HashMap::new();
    let mut used = 0usize;
    let mut clusters = 0u64;

    'streams: while *cursor < affected.len() {
        let id = affected[*cursor];
        let stream = &tree.streams[id as usize];
        debug_assert!(!stream.no_fat_chain);
        let chain = fs.chain(stream.first)?;
        let mut pos = 0;
        while pos < chain.len() {
            if chain[pos] < cut || moved.contains_key(&chain[pos]) {
                pos += 1;
                continue;
            }
            let pred = if pos == 0 {
                Pred::Head { stream: id }
            } else {
                Pred::Fat { index: chain[pos - 1] }
            };
            let linked = pos > 0 && moved.contains_key(&chain[pos - 1]);
            // Group 1: Chain (14) + Bits (10); Group 3: Bits (10) + Free (13); plus Group 2 pred.
            let pred_bytes = if linked {
                0
            } else {
                match pred {
                    Pred::Fat { .. } => 13,
                    Pred::Head { .. } => 56,
                }
            };
            let cost = 14 + 10 + 10 + 13 + pred_bytes;
            if used + cost > BUDGET || clusters >= max_clusters {
                break 'streams;
            }
            let mut len = 1;
            while pos + len < chain.len()
                && chain[pos + len] == chain[pos] + len as u32
                && clusters + (len as u64) < max_clusters
            {
                len += 1;
            }
            let (dst, got) = alloc
                .take(len as u32)
                .ok_or_else(|| Error::Damaged("the free clusters below the cut ran out".into()))?;
            let run = Run {
                src: chain[pos],
                dst,
                len: got,
            };
            for i in 0..got {
                moved.insert(run.src + i, run.dst + i);
            }
            runs.push(run);
            if !linked {
                preds.push((pred, run));
            }
            used += cost;
            clusters += u64::from(got);
            pos += got as usize;
        }
        *cursor += 1;
    }

    if runs.is_empty() {
        return Ok(None);
    }
    finish_move(fs, tree, runs, preds, &moved).map(Some)
}

fn finish_move(
    fs: &Exfat,
    tree: &ExfatTree,
    runs: Vec<Run>,
    preds: Vec<(Pred, Run)>,
    moved: &HashMap<u32, u32>,
) -> Result<MoveBatch> {
    let geo = fs.geometry();
    let mut alloc = Vec::new();
    let mut free = Vec::new();
    let mut fat = Vec::new();
    let mut alloc_runs = Vec::new();
    let mut free_runs = Vec::new();

    for r in &runs {
        let next_raw = fs.fat[(r.src + r.len - 1) as usize];
        let next = moved.get(&next_raw).copied().unwrap_or(next_raw);
        alloc.push(Record::Chain {
            first: r.dst,
            len: r.len,
            last: next,
            loose: true,
        });
        alloc.push(Record::Bits {
            first: r.dst,
            len: r.len,
            set: true,
        });
        free.push(Record::Bits {
            first: r.src,
            len: r.len,
            set: false,
        });
        free.push(Record::Free {
            first: r.src,
            len: r.len,
            last: next_raw,
        });
        alloc_runs.push((r.dst, r.len));
        free_runs.push((r.src, r.len));
        for i in 0..r.len {
            fat.push((r.dst + i, if i + 1 < r.len { r.dst + i + 1 } else { next }));
            fat.push((r.src + i, 0));
        }
    }

    let mut repoint = Vec::new();
    let mut heads = Vec::new();
    let mut chains = HashMap::new();
    for (pred, run) in preds {
        let (old, new) = (run.src, run.dst);
        match pred {
            Pred::Fat { index } => {
                repoint.push(Record::Fat { index, old, new });
                fat.push((index, new));
            }
            Pred::Head { stream } => {
                let s = &tree.streams[stream as usize];
                let Head::Entry { parent, offset } = s.head else {
                    return Err(Error::Volume(
                        "the root directory's first cluster lies past the cut".into(),
                    ));
                };
                let parent_chain = parent_chain_for(fs, tree, &mut chains, parent)?;
                let file_loc = entry_loc(&geo, &parent_chain, moved, offset);
                let stream_loc = entry_loc(&geo, &parent_chain, moved, offset + 32);
                let new_sum = recompute_checksum(s, s.flags, new);
                repoint.push(Record::Field {
                    lba: stream_loc.0,
                    offset: stream_loc.1,
                    kind: FieldKind::ExfatStreamCluster,
                    old: u64::from(old),
                    new: u64::from(new),
                });
                repoint.push(Record::Field {
                    lba: file_loc.0,
                    offset: file_loc.1,
                    kind: FieldKind::ExfatChecksum,
                    old: u64::from(s.checksum),
                    new: u64::from(new_sum),
                });
                heads.push((stream, new, new_sum));
            }
        }
    }

    let step = Step {
        groups: vec![alloc, repoint, free],
    };
    debug_assert!(journal::fits(&step));
    Ok(MoveBatch {
        step,
        copies: copies(&geo, &runs),
        heads,
        fat,
        alloc_runs,
        free_runs,
    })
}

impl MoveBatch {
    pub fn apply_to(&self, fs: &mut Exfat, tree: &mut ExfatTree) {
        for &(i, v) in &self.fat {
            fs.fat[i as usize] = v;
        }
        for &(first, len) in &self.alloc_runs {
            for i in 0..len {
                fs.set_used(first + i, true);
            }
        }
        for &(first, len) in &self.free_runs {
            for i in 0..len {
                fs.set_used(first + i, false);
            }
        }
        for &(id, first, new_sum) in &self.heads {
            let s = &mut tree.streams[id as usize];
            s.first = first;
            s.checksum = new_sum;
            s.entry_set[52..56].copy_from_slice(&first.to_le_bytes());
            s.entry_set[2..4].copy_from_slice(&new_sum.to_le_bytes());
        }
    }
}

fn recompute_checksum(s: &ExfatStream, flags: u8, first: u32) -> u16 {
    let mut buf = s.entry_set.clone();
    buf[33] = flags;
    buf[52..56].copy_from_slice(&first.to_le_bytes());
    entry_set_checksum(&buf)
}

fn copies(geo: &Geometry, runs: &[Run]) -> Vec<(u64, u64, u64)> {
    let mut sorted = runs.to_vec();
    sorted.sort_by_key(|r| r.src);
    let mut out: Vec<(u64, u64, u64)> = Vec::new();
    for r in sorted {
        let (src, dst, n) = (
            geo.cluster_lba(r.src),
            geo.cluster_lba(r.dst),
            u64::from(r.len) * geo.cluster_sectors,
        );
        match out.last_mut() {
            Some((s, d, len)) if *s + *len == src && *d + *len == dst => *len += n,
            _ => out.push((src, dst, n)),
        }
    }
    out
}
