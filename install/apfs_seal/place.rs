// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_btree::Record;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::J_FILE_EXTENT_LEN_MASK;
use switch_apfs_format::fs_records::{FextKey, FextVal};
use switch_apfs_write::extents::Owned;
use zerocopy::IntoBytes;
use zerocopy::little_endian::U64;

use crate::records::{PlannedStream, StreamSource};
use crate::{ReplacementData, SealError};

const BS: u64 = BLOCK_SIZE as u64;

pub enum DataWrite {
    SourceCopy {
        src_paddr: u64,
        dst_paddr: u64,
        blocks: u64,
    },
    Replacement {
        dst_paddr: u64,
        blocks: u64,
        data: ReplacementData,
        size: u64,
        sha256: [u8; 32],
    },
}

pub struct FextRun {
    pub logical: u64,
    pub len: u64,
    pub flags: u8,
    pub rel_dst: u64,
}

pub struct StreamRuns {
    pub stream_id: u64,
    pub runs: Vec<FextRun>,
}

pub struct RelReplacement {
    pub rel_dst: u64,
    pub blocks: u64,
    pub data: ReplacementData,
    pub size: u64,
    pub sha256: [u8; 32],
}

pub struct RelativeData {
    pub src_runs: Vec<(u64, u64, u64)>,
    pub replacements: Vec<RelReplacement>,
    pub stream_runs: Vec<StreamRuns>,
    pub rel_owned: Vec<Owned>,
    pub fext_count: usize,
    pub unique_blocks: u64,
}

pub fn place_relative_data(mut streams: Vec<PlannedStream>) -> Result<RelativeData, SealError> {
    let overflow = || SealError::InvalidSpec("data placement block overflow".into());
    let mut intervals: Vec<(u64, u64)> = Vec::new();
    for s in &streams {
        if let StreamSource::Source(exts) = &s.source {
            for e in exts {
                let end = e.phys_block_num.checked_add(e.blocks()).ok_or_else(overflow)?;
                intervals.push((e.phys_block_num, end));
            }
        }
    }
    intervals.sort_unstable();
    let mut src_runs: Vec<(u64, u64, u64)> = Vec::new();
    let mut rel_cur = 0u64;
    for (start, end) in intervals {
        if let Some(last) = src_runs.last_mut() {
            let last_end = last.0 + last.1;
            if start <= last_end {
                if end > last_end {
                    let extra = end - last_end;
                    last.1 = last.1.checked_add(extra).ok_or_else(overflow)?;
                    rel_cur = rel_cur.checked_add(extra).ok_or_else(overflow)?;
                }
                continue;
            }
        }
        let blocks = end - start;
        let rel_dst = rel_cur;
        rel_cur = rel_cur.checked_add(blocks).ok_or_else(overflow)?;
        src_runs.push((start, blocks, rel_dst));
    }

    streams.sort_by_key(|s| s.stream_id);
    let mut stream_runs = Vec::with_capacity(streams.len());
    let mut replacements = Vec::new();
    let mut rel_owned = Vec::new();
    let mut fext_count = 0usize;

    for s in streams {
        let runs = match s.source {
            StreamSource::Source(exts) => {
                let mut runs: Vec<FextRun> = Vec::new();
                for e in exts {
                    let rel_dst = map_source_paddr(&src_runs, e.phys_block_num);
                    let merged = if let Some(last) = runs.last_mut()
                        && last.flags == e.flags
                        && last.logical.checked_add(last.len) == Some(e.logical_addr)
                        && last.rel_dst.checked_add(last.len / BS) == Some(rel_dst)
                        && let Some(new_len) = last.len.checked_add(e.len)
                        && new_len <= J_FILE_EXTENT_LEN_MASK
                    {
                        last.len = new_len;
                        true
                    } else {
                        false
                    };
                    if !merged {
                        runs.push(FextRun {
                            logical: e.logical_addr,
                            len: e.len,
                            flags: e.flags,
                            rel_dst,
                        });
                    }
                }
                runs
            }
            StreamSource::Replacement { data, size, sha256 } => {
                let rel_dst = rel_cur;
                rel_cur = rel_cur.checked_add(s.blocks).ok_or_else(overflow)?;
                let len = s.blocks.checked_mul(BS).ok_or_else(overflow)?;
                replacements.push(RelReplacement {
                    rel_dst,
                    blocks: s.blocks,
                    data,
                    size,
                    sha256,
                });
                vec![FextRun {
                    logical: 0,
                    len,
                    flags: 0,
                    rel_dst,
                }]
            }
        };
        fext_count += runs.len();
        for r in &runs {
            rel_owned.push(Owned {
                paddr: r.rel_dst,
                blocks: r.len / BS,
                owner: s.owner_oid,
            });
        }
        stream_runs.push(StreamRuns {
            stream_id: s.stream_id,
            runs,
        });
    }

    Ok(RelativeData {
        src_runs,
        replacements,
        stream_runs,
        rel_owned,
        fext_count,
        unique_blocks: rel_cur,
    })
}

fn map_source_paddr(src_runs: &[(u64, u64, u64)], src_paddr: u64) -> u64 {
    let idx = src_runs.partition_point(|&(start, _, _)| start <= src_paddr) - 1;
    let (start, _, rel_dst) = src_runs[idx];
    rel_dst + (src_paddr - start)
}

pub fn encode_fext(stream_id: u64, logical: u64, len: u64, flags: u8, paddr: u64) -> Record {
    let key = FextKey {
        private_id: U64::new(stream_id),
        logical_addr: U64::new(logical),
    };
    let val = FextVal {
        len_and_flags: U64::new(len | (u64::from(flags) << 56)),
        phys_block_num: U64::new(paddr),
    };
    (key.as_bytes().to_vec(), Some(val.as_bytes().to_vec()))
}
