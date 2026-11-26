// SPDX-License-Identifier: GPL-3.0-or-later
//! The only writes this crate makes: entries' types and names, one sector at a time, each followed by a barrier, in an
//! order chosen so a power cut between any two leaves a card both readers can take:
//!
//! 0. a primary header still holding an older array's CRC is brought up to its array (the kernel's view is unchanged);
//! 1. the backup array's changed sectors, then 2. the backup header;
//! 3. the primary array's changed sectors, the target's last, then 4. the primary header.
//!
//! The kernel's root rule reads only the primary array, so it sees each step land at one sector write in 3, and a
//! sector that holds only demotions lands before the target's. And at every point one of the two copies matches its own
//! header's CRC, which is what `Tables::read` needs to know which copy is mid-write.

use crate::card::{Card, Plan};
use crate::device::Device;
use crate::entry::{APFS_TYPE, DEMOTED_NAME, Entry, LINUX_TYPE, ROOT_NAME, SETUP_NAME};
use crate::error::Error;
use crate::gpt::{Copy, SECTOR};

impl Card {
    /// Before the APFS write: the target named `iOS-setup` and, if it was APFS-typed, retyped Linux, so the kernel
    /// never counts a half-written container as a root. Other roots keep booting meanwhile.
    pub fn mark<D: Device>(&mut self, dev: &mut D, plan: &Plan) -> Result<usize, Error> {
        let t = self.planned(plan)?;
        let changes = [(t, unrooted(&self.entries[t as usize], SETUP_NAME))];
        self.apply(dev, &changes, t)
    }

    /// After the APFS write: every other root and unfinished install back to `userdata`, then the target `iOS` and
    /// APFS-typed, name and type in one write.
    pub fn finish<D: Device>(&mut self, dev: &mut D, plan: &Plan) -> Result<usize, Error> {
        let t = self.planned(plan)?;
        let mut changes: Vec<(u32, Entry)> = plan
            .others
            .iter()
            .map(|&i| (i, unrooted(&self.entries[i as usize], DEMOTED_NAME)))
            .collect();
        changes.push((t, self.entries[t as usize].retyped(APFS_TYPE, ROOT_NAME)));
        self.apply(dev, &changes, t)
    }

    fn planned(&self, plan: &Plan) -> Result<u32, Error> {
        let t = plan.target;
        let e = self.entries.get(t.index as usize).ok_or(Error::NotATarget(t.index))?;
        if e.first() != t.first || e.sectors() != t.sectors {
            return Err(Error::NotATarget(t.index));
        }
        Ok(t.index)
    }

    /// Writes the sectors that differ from `changes` applied to the primary array, in the module's order; the number
    /// of sectors written.
    fn apply<D: Device>(&mut self, dev: &mut D, changes: &[(u32, Entry)], last: u32) -> Result<usize, Error> {
        let mut entries = self.entries.clone();
        for &(i, e) in changes {
            entries[i as usize] = e;
        }
        let mut writes = 0;
        let primary = &mut self.tables.primary;
        if !primary.consistent() {
            let header = primary.header.with_array_crc(crc32fast::hash(primary.declared()));
            writes += put(dev, primary.header_lba, &header.0)?;
            primary.header = header;
        }
        writes += update(dev, &mut self.tables.backup, &entries, None)?;
        writes += update(dev, &mut self.tables.primary, &entries, Some(last))?;
        self.entries = entries;
        Ok(writes)
    }
}

/// `entry` renamed, and retyped Linux if APFS-typed: neither half of the kernel's root rule holds for it.
fn unrooted(entry: &Entry, name: &str) -> Entry {
    let type_guid = if entry.type_guid() == APFS_TYPE {
        LINUX_TYPE
    } else {
        entry.type_guid()
    };
    entry.retyped(type_guid, name)
}

/// One copy's array sectors brought to `entries` (the sector holding entry `last` after the rest), then its header.
fn update<D: Device>(dev: &mut D, copy: &mut Copy, entries: &[Entry], last: Option<u32>) -> Result<usize, Error> {
    let count = copy.header.entry_count();
    let mut writes = 0;
    for (lba, sector) in copy_writes(copy, entries, count, last) {
        writes += put(dev, lba, &sector)?;
    }
    Ok(writes)
}

/// The sectors that bring one copy to `entries` with `count` declared, in `update`'s order, as (LBA, contents);
/// `copy` is left as they leave it. The array must already span `count` entries.
pub(crate) fn copy_writes(
    copy: &mut Copy,
    entries: &[Entry],
    count: u32,
    last: Option<u32>,
) -> Vec<(u64, [u8; SECTOR])> {
    let mut new = copy.clone();
    for (i, e) in entries.iter().enumerate() {
        new.set_entry(i as u32, e);
    }
    let last_sector = last.map(|i| new.entry_at(i) / SECTOR);
    let mut changed: Vec<usize> = (0..new.array.len() / SECTOR)
        .filter(|&s| new.array[s * SECTOR..][..SECTOR] != copy.array[s * SECTOR..][..SECTOR])
        .collect();
    changed.sort_by_key(|&s| (Some(s) == last_sector, s));
    let mut writes = Vec::new();
    for s in changed {
        let sector: [u8; SECTOR] = new.array[s * SECTOR..][..SECTOR].try_into().unwrap();
        writes.push((copy.header.entry_lba() + s as u64, sector));
        copy.array[s * SECTOR..][..SECTOR].copy_from_slice(&sector);
    }
    let declared = count as usize * copy.header.entry_size() as usize;
    let header = copy
        .header
        .with_count_and_crc(count, crc32fast::hash(&copy.array[..declared]));
    if header != copy.header {
        writes.push((copy.header_lba, header.0));
        copy.header = header;
    }
    writes
}

fn put<D: Device>(dev: &mut D, lba: u64, sector: &[u8]) -> Result<usize, Error> {
    dev.write_sectors(lba, sector)?;
    dev.barrier()?;
    Ok(1)
}
