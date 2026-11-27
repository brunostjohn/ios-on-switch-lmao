// SPDX-License-Identifier: GPL-3.0-or-later
//! Table changes a caller journals and writes itself (switch_partition): the sectors, in write.rs's order, unwritten.

use crate::card::Card;
use crate::device::Device;
use crate::entry::Entry;
use crate::error::Error;
use crate::gpt::SECTOR;
use crate::write::copy_writes;

impl Card {
    pub fn mbr(&self) -> [u8; SECTOR] {
        self.mbr
    }

    /// The header's first and last usable LBA.
    pub fn usable(&self) -> (u64, u64) {
        let h = &self.tables.primary.header;
        (h.first_usable(), h.last_usable())
    }

    pub fn disk_guid(&self) -> [u8; 16] {
        self.tables.primary.header.disk_guid()
    }

    pub fn check_entry_count(&self, count: u32) -> Result<(), Error> {
        let capacity = self.sector_count();
        let primary = &self.tables.primary.header;
        let backup = &self.tables.backup.header;
        primary
            .with_count_and_crc(count, primary.array_crc())
            .check(capacity, false)?;
        backup
            .with_count_and_crc(count, backup.array_crc())
            .check(capacity, true)?;
        Ok(())
    }

    /// The sector writes that put `changes` into both copies with `count` entries declared, in write.rs's order
    /// (backup array, backup header, primary array with the highest changed index's sector last, primary header).
    /// The result must still be a table `Card::open` takes. Nothing is written.
    pub fn table_writes<D: Device>(
        &self,
        dev: &mut D,
        changes: &[(u32, Entry)],
        count: u32,
    ) -> Result<Vec<(u64, [u8; SECTOR])>, Error> {
        if self.torn() {
            return Err(Error::CopiesDiffer("a cut-short write is still in the tables"));
        }
        if count < self.tables.count() || changes.iter().any(|&(i, _)| i >= count) {
            return Err(Error::Entry {
                index: count,
                why: "a change past the declared entries, or fewer entries than now",
            });
        }
        self.check_entry_count(count)?;
        let mut tables = self.tables.clone();
        tables.primary.extend_to(dev, count)?;
        tables.backup.extend_to(dev, count)?;
        let mut entries: Vec<Entry> = (0..count).map(|i| tables.primary.entry(i)).collect();
        for &(i, e) in changes {
            entries[i as usize] = e;
        }
        let last = changes.iter().map(|&(i, _)| i).max();
        let mut writes = copy_writes(&mut tables.backup, &entries, count, None);
        writes.extend(copy_writes(&mut tables.primary, &entries, count, last));
        let capacity = self.sector_count();
        tables.primary.header.check(capacity, false)?;
        tables.backup.header.check(capacity, true)?;
        if !tables.clean() {
            return Err(Error::CopiesDiffer("the written GPT copies do not match"));
        }
        Card {
            mbr: self.mbr,
            tables,
            entries,
        }
        .check_entries()?;
        Ok(writes)
    }
}
