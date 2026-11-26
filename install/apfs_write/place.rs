// SPDX-License-Identifier: GPL-3.0-or-later
//! Where every new data block goes. Source extents are placed in source block order, so phase C's second writes
//! climb through the target as its pass through the source does; a block two streams of one volume share is placed
//! once, and one that two volumes copy is refused, since APFS never shares a block across volumes. New content follows
//! in tree order, and a `Fill` stream gets one contiguous run. A stream nothing reads gets no blocks.

use std::ops::Range;
use std::sync::Arc;

use switch_apfs_alloc::Bitmap;
use switch_apfs_format::BLOCK_SIZE;

use crate::WriteError;
use crate::content::{Content, Fill, SourceExtent};
use crate::tree::Tree;

const BS: u64 = BLOCK_SIZE as u64;

/// The free space left after the container's own objects, handed out lowest first.
pub struct FreeRuns {
    runs: Vec<Range<u64>>,
    first: usize,
}

impl FreeRuns {
    pub fn from_bitmap(used: &Bitmap) -> Self {
        let mut runs = Vec::new();
        let mut at = 0;
        for run in used.set_runs() {
            if run.start > at {
                runs.push(at..run.start);
            }
            at = run.end;
        }
        if at < used.len() {
            runs.push(at..used.len());
        }
        Self { runs, first: 0 }
    }

    /// `count` blocks from the lowest free runs, split across runs where one is too short.
    pub fn take(&mut self, mut count: u64) -> Result<Vec<Range<u64>>, WriteError> {
        let wanted = count;
        let mut out = Vec::new();
        while count > 0 {
            let run = self.runs.get_mut(self.first).ok_or(WriteError::Full(wanted))?;
            let n = count.min(run.end - run.start);
            out.push(run.start..run.start + n);
            run.start += n;
            count -= n;
            while self.runs.get(self.first).is_some_and(|r| r.is_empty()) {
                self.first += 1;
            }
        }
        Ok(out)
    }

    /// The lowest run of `count` blocks in one piece.
    pub fn take_contiguous(&mut self, count: u64) -> Result<u64, WriteError> {
        let i = (self.first..self.runs.len())
            .find(|&i| self.runs[i].end - self.runs[i].start >= count)
            .ok_or(WriteError::Full(count))?;
        let at = self.runs[i].start;
        self.runs[i].start += count;
        while self.runs.get(self.first).is_some_and(|r| r.is_empty()) {
            self.first += 1;
        }
        Ok(at)
    }
}

/// Source blocks `src..src + blocks` also go to `dst..`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Relocation {
    pub src: u64,
    pub dst: u64,
    pub blocks: u64,
}

#[derive(Clone)]
pub enum NewContent {
    /// The stream's bytes from `offset`, zero-padded to whole blocks.
    Bytes {
        bytes: Arc<[u8]>,
        offset: usize,
    },
    Fill(Arc<dyn Fill>),
}

#[derive(Clone)]
pub struct NewData {
    pub dst: u64,
    pub blocks: u64,
    pub content: NewContent,
}

/// A `FILE_EXTENT` as the target volume stores it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlacedExtent {
    pub logical: u64,
    pub len: u64,
    pub flags: u8,
    /// 0 for a hole.
    pub paddr: u64,
    pub crypto_id: u64,
}

pub struct Placement {
    pub relocations: Vec<Relocation>,
    pub new_data: Vec<NewData>,
    /// Per volume, per stream.
    pub extents: Vec<Vec<Vec<PlacedExtent>>>,
    pub taken: Vec<Range<u64>>,
}

pub fn place(trees: &[&Tree], free: &mut FreeRuns) -> Result<Placement, WriteError> {
    let used: Vec<Vec<bool>> = trees.iter().map(|t| t.used_streams()).collect();
    let mut taken = Vec::new();
    let relocations = relocate(trees, &used, free, &mut taken)?;
    let mut new_data = Vec::new();
    let mut extents = Vec::with_capacity(trees.len());
    for (tree, used) in trees.iter().zip(&used) {
        let mut per_stream = Vec::with_capacity(tree.streams.len());
        for (stream, &used) in tree.streams.iter().zip(used) {
            if !used {
                per_stream.push(Vec::new());
                continue;
            }
            let blocks = stream.size.div_ceil(BS);
            let placed = match &stream.content {
                Content::Source(source) => {
                    let mut out = Vec::new();
                    for e in source.iter() {
                        translate(&relocations, e, &mut out)?;
                    }
                    out
                }
                Content::Bytes(bytes) => {
                    let mut out = Vec::new();
                    let mut logical = 0u64;
                    for run in free.take(blocks)? {
                        let n = run.end - run.start;
                        new_data.push(NewData {
                            dst: run.start,
                            blocks: n,
                            content: NewContent::Bytes {
                                bytes: bytes.clone(),
                                offset: logical as usize,
                            },
                        });
                        out.push(dense(logical, n, run.start));
                        logical += n * BS;
                        taken.push(run);
                    }
                    out
                }
                Content::Fill(fill) => {
                    if blocks == 0 {
                        Vec::new()
                    } else {
                        let at = free.take_contiguous(blocks)?;
                        new_data.push(NewData {
                            dst: at,
                            blocks,
                            content: NewContent::Fill(fill.clone()),
                        });
                        taken.push(at..at + blocks);
                        vec![dense(0, blocks, at)]
                    }
                }
            };
            per_stream.push(placed);
        }
        extents.push(per_stream);
    }
    new_data.sort_by_key(|d| d.dst);
    Ok(Placement {
        relocations,
        new_data,
        extents,
        taken,
    })
}

fn dense(logical: u64, blocks: u64, paddr: u64) -> PlacedExtent {
    PlacedExtent {
        logical,
        len: blocks * BS,
        flags: 0,
        paddr,
        crypto_id: 0,
    }
}

/// Every source block some stream reads, merged into runs in source order, each given target blocks.
fn relocate(
    trees: &[&Tree],
    used: &[Vec<bool>],
    free: &mut FreeRuns,
    taken: &mut Vec<Range<u64>>,
) -> Result<Vec<Relocation>, WriteError> {
    let mut wanted: Vec<(Range<u64>, usize)> = Vec::new();
    for (volume, (tree, used)) in trees.iter().zip(used).enumerate() {
        for (stream, _) in tree.streams.iter().zip(used).filter(|(_, u)| **u) {
            if let Content::Source(extents) = &stream.content {
                check_source(stream.size, extents)?;
                let copied = extents.iter().filter(|e| e.paddr != 0);
                wanted.extend(copied.map(|e| (e.paddr..e.paddr + e.blocks(), volume)));
            }
        }
    }
    wanted.sort_by_key(|(r, v)| (r.start, r.end, *v));
    let mut reach = vec![0u64; trees.len()];
    for (r, v) in &wanted {
        if let Some(other) = (0..trees.len()).find(|&u| u != *v && reach[u] > r.start) {
            return Err(WriteError::Refused(format!(
                "source block {} is copied into volumes {other} and {v}; APFS doesn't share blocks across volumes",
                r.start
            )));
        }
        reach[*v] = reach[*v].max(r.end);
    }
    let mut merged: Vec<Range<u64>> = Vec::new();
    for (r, _) in wanted {
        match merged.last_mut() {
            Some(last) if r.start <= last.end => last.end = last.end.max(r.end),
            _ => merged.push(r),
        }
    }
    let mut out = Vec::new();
    for run in merged {
        let mut src = run.start;
        for piece in free.take(run.end - run.start)? {
            let blocks = piece.end - piece.start;
            out.push(Relocation {
                src,
                dst: piece.start,
                blocks,
            });
            src += blocks;
            taken.push(piece);
        }
    }
    Ok(out)
}

/// A source stream's extents must follow each other in logical order without overlapping, and end within the block
/// that holds the stream's last byte.
fn check_source(size: u64, extents: &[SourceExtent]) -> Result<(), WriteError> {
    let end = size.next_multiple_of(BS);
    let mut at = 0;
    for e in extents {
        if e.logical < at || e.logical + e.len > end {
            return Err(WriteError::Source(
                "a source extent overlaps another or passes the end of its stream",
            ));
        }
        at = e.logical + e.len;
    }
    Ok(())
}

/// A source extent at its target blocks: one extent per relocation piece it spans.
fn translate(relocations: &[Relocation], e: &SourceExtent, out: &mut Vec<PlacedExtent>) -> Result<(), WriteError> {
    if e.paddr == 0 {
        out.push(PlacedExtent {
            logical: e.logical,
            len: e.len,
            flags: e.flags,
            paddr: 0,
            crypto_id: e.crypto_id,
        });
        return Ok(());
    }
    let mut src = e.paddr;
    let mut logical = e.logical;
    let mut left = e.len;
    let end = e.paddr + e.blocks();
    while src < end {
        let i = relocations.partition_point(|r| r.src <= src);
        let r = i
            .checked_sub(1)
            .map(|i| relocations[i])
            .filter(|r| src < r.src + r.blocks)
            .ok_or(WriteError::Source("a source extent outside every relocation"))?;
        let blocks = (r.src + r.blocks).min(end) - src;
        let len = left.min(blocks * BS);
        out.push(PlacedExtent {
            logical,
            len,
            flags: e.flags,
            paddr: r.dst + (src - r.src),
            crypto_id: e.crypto_id,
        });
        src += blocks;
        logical += len;
        left -= len;
    }
    Ok(())
}

/// Physical bytes of a stream's extents, holes excluded.
pub fn physical_bytes(extents: &[PlacedExtent]) -> u64 {
    extents
        .iter()
        .filter(|e| e.paddr != 0)
        .map(|e| e.len.div_ceil(BS) * BS)
        .sum()
}
