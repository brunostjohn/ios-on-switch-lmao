// SPDX-License-Identifier: GPL-3.0-or-later
//! Sectors read once for a group of changes, edited in memory, and written back in contiguous runs.

use std::collections::BTreeMap;

use switch_card::{Device, SECTOR};

use crate::error::Result;

/// The longest single request, in sectors (8 MiB).
pub const MAX_REQUEST: u64 = 16384;

type Sector = [u8; SECTOR];

#[derive(Default)]
pub struct Cache {
    sectors: BTreeMap<u64, (Box<Sector>, Box<Sector>)>,
}

impl Cache {
    /// Reads every sector of `ranges` (LBA, count) not held yet, merged into long requests.
    pub fn load<D: Device>(&mut self, dev: &mut D, ranges: impl IntoIterator<Item = (u64, u64)>) -> Result<()> {
        let mut wanted: Vec<u64> = Vec::new();
        for (lba, count) in ranges {
            wanted.extend((lba..lba + count).filter(|l| !self.sectors.contains_key(l)));
        }
        wanted.sort_unstable();
        wanted.dedup();
        let mut buf = Vec::new();
        for (first, count) in runs(&wanted) {
            buf.resize(count as usize * SECTOR, 0);
            dev.read_sectors(first, &mut buf)?;
            for (i, s) in buf.chunks(SECTOR).enumerate() {
                let sector: Box<Sector> = Box::new(s.try_into().unwrap());
                self.sectors.insert(first + i as u64, (sector.clone(), sector));
            }
        }
        Ok(())
    }

    pub fn get<D: Device>(&mut self, dev: &mut D, lba: u64) -> Result<&mut Sector> {
        if !self.sectors.contains_key(&lba) {
            self.load(dev, [(lba, 1)])?;
        }
        Ok(&mut self.sectors.get_mut(&lba).unwrap().1)
    }

    /// Writes the sectors that differ from what was read; the number of write requests.
    pub fn flush<D: Device>(self, dev: &mut D) -> Result<usize> {
        let dirty: Vec<(u64, Box<Sector>)> = self
            .sectors
            .into_iter()
            .filter(|(_, (was, now))| was != now)
            .map(|(lba, (_, now))| (lba, now))
            .collect();
        let lbas: Vec<u64> = dirty.iter().map(|(l, _)| *l).collect();
        let mut at = 0;
        let mut writes = 0;
        let mut buf = Vec::new();
        for (first, count) in runs(&lbas) {
            buf.clear();
            for (_, s) in &dirty[at..at + count as usize] {
                buf.extend_from_slice(&s[..]);
            }
            dev.write_sectors(first, &buf)?;
            at += count as usize;
            writes += 1;
        }
        Ok(writes)
    }
}

/// Sorted, distinct LBAs as (first, count) runs of at most `MAX_REQUEST`.
fn runs(lbas: &[u64]) -> Vec<(u64, u64)> {
    let mut out: Vec<(u64, u64)> = Vec::new();
    for &lba in lbas {
        match out.last_mut() {
            Some((first, count)) if *first + *count == lba && *count < MAX_REQUEST => *count += 1,
            _ => out.push((lba, 1)),
        }
    }
    out
}
