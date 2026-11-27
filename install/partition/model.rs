// SPDX-License-Identifier: GPL-3.0-or-later
//! The volume's files and directories as the relocation needs them: each one's first cluster and where that number
//! is kept, so a move can update it wherever that place sits by then.

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Head {
    /// The boot sector holds it (the root directory).
    Root,
    /// A directory entry at byte `offset` of directory `parent`'s data: FAT32's short entry, exFAT's file entry.
    Entry { parent: u32, offset: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stream {
    pub first: u32,
    pub dir: bool,
    pub head: Head,
}

/// What a walk of the whole tree found.
pub struct Tree {
    pub streams: Vec<Stream>,
    /// Each stream's highest cluster.
    pub highest: Vec<u32>,
    /// Every cluster some stream owns.
    pub owned: Bits,
    /// One past the highest cluster that is in use but owned by nothing (lost); 2 if there is none.
    pub lost_end: u32,
}

impl Tree {
    /// Streams with a cluster at or past `cut`, directories first.
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
}

/// One bit per cluster.
pub struct Bits(Vec<u64>);

impl Bits {
    pub fn new(len: u32) -> Bits {
        Bits(vec![0; (len as usize).div_ceil(64)])
    }

    pub fn get(&self, i: u32) -> bool {
        self.0[i as usize / 64] >> (i % 64) & 1 != 0
    }

    /// Sets bit `i`; false if it was set already.
    pub fn set(&mut self, i: u32) -> bool {
        let was = self.get(i);
        self.0[i as usize / 64] |= 1 << (i % 64);
        !was
    }
}

/// The lowest cut where every used cluster fits below: the first `x` with as many used-or-free clusters below it as
/// there are used ones in all, and past every cluster `pinned` (lost ones, which a shrink can't move).
pub fn lowest_cut(end: u32, used: impl Fn(u32) -> bool, free: impl Fn(u32) -> bool, pinned: u32) -> u32 {
    let total = (2..end).filter(|&c| used(c)).count() as u64;
    let mut room = 0u64;
    let mut x = 2;
    while x < end && room < total {
        if used(x) || free(x) {
            room += 1;
        }
        x += 1;
    }
    x.max(pinned)
}
