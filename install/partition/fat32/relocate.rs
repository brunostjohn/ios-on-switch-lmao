// SPDX-License-Identifier: GPL-3.0-or-later
//! One batch of FAT32 cluster moves out of the cut: which runs go where, the data copies, and the step that makes
//! them the volume's in three groups: the new chains, then every reference to a moved run, then the old chains freed.
//! Until the second group lands the files read from their old clusters; from then on, from the new.

use std::collections::HashMap;

use super::boot::ROOT_CLUS;
use super::{Fat32, MASK};
use crate::error::{Error, Result};
use crate::journal::{self, FieldKind, Record, Step};
use crate::model::{Head, Tree};

/// A batch moves at most this much data, so a cut repeats little.
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

pub struct Batch {
    pub step: Step,
    /// (source LBA, destination LBA, sectors), sorted and merged.
    pub copies: Vec<(u64, u64, u64)>,
    pub heads: Vec<(u32, u32)>,
    pub fat: Vec<(u32, u32)>,
}

/// Free clusters below the cut, handed out from the longest free runs first so moved files stay in few pieces. Each
/// batch builds its own from the FAT as it stands, so a run restarted after a cut plans the batches an uncut one would.
struct Allocator {
    extents: Vec<(u32, u32)>,
    at: usize,
}

impl Allocator {
    fn below(fs: &Fat32, cut: u32) -> Allocator {
        let mut extents = Vec::new();
        let mut c = 2;
        while c < cut {
            if fs.fat[c as usize] != 0 {
                c += 1;
                continue;
            }
            let start = c;
            while c < cut && fs.fat[c as usize] == 0 {
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

/// The next batch, or None once no stream of `affected` has a cluster at or past `cut`. `cursor` indexes the first
/// of them that may still have one; the planner moves it on.
pub fn plan(fs: &Fat32, tree: &Tree, affected: &[u32], cut: u32, cursor: &mut usize) -> Result<Option<Batch>> {
    let mut alloc = Allocator::below(fs, cut);
    let cluster_bytes = fs.bpb.cluster_sectors * 512;
    let max_clusters = (BATCH_BYTES / cluster_bytes).max(1);
    let mut runs: Vec<Run> = Vec::new();
    let mut preds: Vec<(Pred, Run)> = Vec::new();
    let mut moved: HashMap<u32, u32> = HashMap::new();
    let mut used = 0usize;
    let mut clusters = 0u64;
    'streams: while *cursor < affected.len() {
        let id = affected[*cursor];
        let stream = &tree.streams[id as usize];
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
            let cost = 14
                + 13
                + if linked {
                    0
                } else {
                    pred_cost(&pred, stream.dir, &stream.head)
                };
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
    finish(fs, tree, runs, preds, &moved).map(Some)
}

fn pred_cost(pred: &Pred, dir: bool, head: &Head) -> usize {
    match pred {
        Pred::Fat { .. } => 13,
        Pred::Head { .. } => (if *head == Head::Root { 56 } else { 28 }) + if dir { 10 } else { 0 },
    }
}

fn finish(
    fs: &Fat32,
    tree: &Tree,
    runs: Vec<Run>,
    preds: Vec<(Pred, Run)>,
    moved: &HashMap<u32, u32>,
) -> Result<Batch> {
    let geo = fs.geometry();
    let mut alloc = Vec::new();
    let mut free = Vec::new();
    let mut fat = Vec::new();
    for r in &runs {
        let next_raw = fs.fat[(r.src + r.len - 1) as usize];
        let next = moved.get(&next_raw).copied().unwrap_or(next_raw);
        alloc.push(Record::Chain {
            first: r.dst,
            len: r.len,
            last: next,
            loose: false,
        });
        free.push(Record::Free {
            first: r.src,
            len: r.len,
            last: next_raw,
        });
        for i in 0..r.len {
            fat.push((r.dst + i, if i + 1 < r.len { r.dst + i + 1 } else { next }));
            fat.push((r.src + i, 0));
        }
    }
    let mut backup_root = Vec::new();
    let mut repoint = Vec::new();
    let mut dot_dots = Vec::new();
    let mut heads = Vec::new();
    let mut chains: HashMap<u32, Vec<u32>> = HashMap::new();
    for (pred, run) in preds {
        let (old, new) = (run.src, run.dst);
        match pred {
            Pred::Fat { index } => {
                repoint.push(Record::Fat { index, old, new });
                fat.push((index, new));
            }
            Pred::Head { stream } => {
                let s = &tree.streams[stream as usize];
                match s.head {
                    Head::Root => {
                        if fs.bpb.backup_boot != 0 {
                            backup_root.push(Record::Field {
                                lba: fs.first + fs.bpb.backup_boot,
                                offset: ROOT_CLUS,
                                kind: FieldKind::Le32,
                                old: u64::from(old),
                                new: u64::from(new),
                            });
                        }
                        repoint.push(Record::Field {
                            lba: fs.first,
                            offset: ROOT_CLUS,
                            kind: FieldKind::Le32,
                            old: u64::from(old),
                            new: u64::from(new),
                        });
                    }
                    Head::Entry { parent, offset } => {
                        if let std::collections::hash_map::Entry::Vacant(e) = chains.entry(parent) {
                            e.insert(fs.chain(tree.streams[parent as usize].first)?);
                        }
                        let parent_chain = &chains[&parent];
                        let cb = geo.cluster_bytes();
                        let here = parent_chain[(offset / cb) as usize];
                        let cluster = moved.get(&here).copied().unwrap_or(here);
                        let within = offset % cb;
                        repoint.push(Record::Field {
                            lba: geo.cluster_lba(cluster) + within / 512,
                            offset: (within % 512) as u16,
                            kind: FieldKind::Fat32Cluster,
                            old: u64::from(old),
                            new: u64::from(new),
                        });
                    }
                }
                if s.dir {
                    dot_dots.push(Record::DotDot {
                        old,
                        new,
                        root: s.head == Head::Root,
                    });
                }
                heads.push((stream, new));
            }
        }
    }
    repoint.extend(dot_dots);
    debug_assert!(fat.iter().all(|&(_, v)| v & !MASK == 0));
    let mut groups = vec![alloc];
    if !backup_root.is_empty() {
        groups.push(backup_root);
    }
    groups.push(repoint);
    groups.push(free);
    let step = Step { groups };
    debug_assert!(journal::fits(&step));
    Ok(Batch {
        step,
        copies: copies(&geo, &runs),
        heads,
        fat,
    })
}

fn copies(geo: &crate::apply::Geometry, runs: &[Run]) -> Vec<(u64, u64, u64)> {
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

impl Batch {
    pub fn apply_to(&self, fs: &mut Fat32, tree: &mut Tree) {
        for &(i, v) in &self.fat {
            fs.fat[i as usize] = v;
        }
        for &(s, first) in &self.heads {
            tree.streams[s as usize].first = first;
        }
    }
}
