// SPDX-License-Identifier: GPL-3.0-or-later
//! Every file and directory on a FAT32 volume, found from the root, each chain checked to be whole and to share no
//! cluster with another; and every used cluster past the cut owned by one of them.

use std::collections::VecDeque;

use switch_card::{Device, SECTOR};

use super::Fat32;
use crate::cache::MAX_REQUEST;
use crate::error::{Error, Result};
use crate::model::{Bits, Head, Stream, Tree};

pub const DOT: &[u8; 11] = b".          ";
pub const DOT_DOT: &[u8; 11] = b"..         ";

pub fn walk<D: Device>(dev: &mut D, fs: &Fat32) -> Result<Tree> {
    let end = fs.cluster_end();
    let mut seen = Bits::new(end);
    let root = Stream {
        first: fs.bpb.root,
        dir: true,
        head: Head::Root,
    };
    let mut streams = vec![root];
    let mut highest = Vec::new();
    let mut queue = VecDeque::from([0u32]);
    let mut claim = |id: u32, first: u32, seen: &mut Bits| -> Result<Vec<u32>> {
        let chain = fs.chain(first)?;
        for &c in &chain {
            if !seen.set(c) {
                return Err(Error::Damaged(format!("cluster {c} belongs to two chains")));
            }
        }
        if highest.len() <= id as usize {
            highest.resize(id as usize + 1, 0);
        }
        highest[id as usize] = chain.iter().copied().max().unwrap_or(0);
        Ok(chain)
    };
    while let Some(id) = queue.pop_front() {
        let dir = streams[id as usize].clone();
        let chain = claim(id, dir.first, &mut seen)?;
        let data = read_clusters(dev, fs, &chain)?;
        if let Head::Entry { parent, .. } = dir.head {
            let parent_first = if parent == 0 { 0 } else { streams[parent as usize].first };
            check_dots(&data, dir.first, parent_first, fs.bpb.root)?;
        }
        for (i, entry) in data.chunks(32).enumerate() {
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
            let child = streams.len() as u32;
            streams.push(Stream {
                first,
                dir: is_dir,
                head: Head::Entry {
                    parent: id,
                    offset: i as u64 * 32,
                },
            });
            if is_dir {
                queue.push_back(child);
            } else {
                let chain = claim(child, first, &mut seen)?;
                let expected = file_size.div_ceil(fs.bpb.cluster_sectors * SECTOR as u64);
                if chain.len() as u64 != expected {
                    return Err(Error::Damaged(format!(
                        "file at cluster {first} has {} clusters for {file_size} bytes",
                        chain.len()
                    )));
                }
            }
        }
    }
    let lost_end = (2..end)
        .rev()
        .find(|&c| fs.is_used(c) && !seen.get(c))
        .map_or(2, |c| c + 1);
    highest.resize(streams.len(), 0);
    Ok(Tree {
        streams,
        highest,
        owned: seen,
        lost_end,
    })
}

pub fn cluster_of(entry: &[u8]) -> u32 {
    let half = |at: usize| u32::from(u16::from_le_bytes([entry[at], entry[at + 1]]));
    half(20) << 16 | half(26)
}

/// A subdirectory starts with `.` (itself) and `..` (its parent, 0 for the root, or the root's cluster as some
/// formatters write it).
fn check_dots(data: &[u8], own: u32, parent: u32, root: u32) -> Result<()> {
    let ok = data.len() >= 64 && &data[..11] == DOT && cluster_of(&data[..32]) == own && &data[32..43] == DOT_DOT && {
        let up = cluster_of(&data[32..64]);
        up == parent || (parent == 0 && up == root)
    };
    if ok {
        Ok(())
    } else {
        Err(Error::Damaged(format!(
            "the directory at cluster {own} doesn't start with its . and .. entries"
        )))
    }
}

/// The clusters of `chain`, read in runs of consecutive clusters.
pub fn read_clusters<D: Device>(dev: &mut D, fs: &Fat32, chain: &[u32]) -> Result<Vec<u8>> {
    let geo = fs.geometry();
    let per = geo.cluster_sectors;
    let mut data = vec![0u8; chain.len() * (per as usize) * SECTOR];
    let mut i = 0;
    while i < chain.len() {
        let mut n = 1;
        while i + n < chain.len() && chain[i + n] == chain[i] + n as u32 && (n as u64 + 1) * per <= MAX_REQUEST {
            n += 1;
        }
        let at = i * per as usize * SECTOR;
        let len = n * per as usize * SECTOR;
        dev.read_sectors(geo.cluster_lba(chain[i]), &mut data[at..at + len])?;
        i += n;
    }
    Ok(data)
}
