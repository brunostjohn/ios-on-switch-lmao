// SPDX-License-Identifier: GPL-3.0-or-later
//! Both GPT copies as raw sectors, held to the profile `switch_gpt_scan` (diagnostics/switch/storage/partition.c)
//! accepts, and the one kind of disagreement between the copies that `switch_card`'s own ordered writes leave.

use crate::device::Device;
use crate::entry::{ENTRY_BYTES, Entry};
use crate::error::Error;

pub const SECTOR: usize = 512;
const HEADER_BYTES: usize = 92;
const MAX_ENTRIES: u32 = 128;
const MAX_ARRAY_BYTES: u64 = 64 * 1024;
/// Hekate declares only the entries it made but reserves 16 KiB for the array; the strict scan insists on it.
const RESERVED_ARRAY_SECTORS: u64 = 32;

#[derive(Clone, PartialEq, Eq)]
pub struct Header(pub [u8; SECTOR]);

impl Header {
    fn u32_at(&self, at: usize) -> u32 {
        u32::from_le_bytes(self.0[at..at + 4].try_into().unwrap())
    }
    fn u64_at(&self, at: usize) -> u64 {
        u64::from_le_bytes(self.0[at..at + 8].try_into().unwrap())
    }
    pub fn first_usable(&self) -> u64 {
        self.u64_at(40)
    }
    pub fn last_usable(&self) -> u64 {
        self.u64_at(48)
    }
    pub fn entry_lba(&self) -> u64 {
        self.u64_at(72)
    }
    pub fn entry_count(&self) -> u32 {
        self.u32_at(80)
    }
    pub fn entry_size(&self) -> u32 {
        self.u32_at(84)
    }
    pub fn array_crc(&self) -> u32 {
        self.u32_at(88)
    }

    fn computed_crc(&self) -> u32 {
        let mut h = self.0;
        h[16..20].fill(0);
        crc32fast::hash(&h[..HEADER_BYTES])
    }

    /// The same header for an array whose CRC is `array_crc`: only bytes 88..92 and the header CRC change.
    pub fn with_array_crc(&self, array_crc: u32) -> Header {
        let mut h = Header(self.0);
        h.0[88..92].copy_from_slice(&array_crc.to_le_bytes());
        let crc = h.computed_crc();
        h.0[16..20].copy_from_slice(&crc.to_le_bytes());
        h
    }

    fn check(&self, capacity: u64, backup: bool) -> Result<(), Error> {
        let bad = |why: &'static str| Err(Error::Header { backup, why });
        let h = &self.0;
        if &h[..8] != b"EFI PART" || self.u32_at(8) != 0x10000 || self.u32_at(12) != HEADER_BYTES as u32 {
            return bad("not a GPT v1 header of 92 bytes");
        }
        if self.u32_at(20) != 0 || h[HEADER_BYTES..].iter().any(|&b| b != 0) {
            return bad("reserved bytes are not zero");
        }
        if self.u32_at(16) != self.computed_crc() {
            return bad("header CRC");
        }
        let (mine, other) = if backup { (capacity - 1, 1) } else { (1, capacity - 1) };
        if self.u64_at(24) != mine || self.u64_at(32) != other {
            return bad("its own or the other header's LBA");
        }
        if h[56..72].iter().all(|&b| b == 0) {
            return bad("zero disk GUID");
        }
        let (first, last) = (self.first_usable(), self.last_usable());
        if first < 34 || first > last || last >= capacity - 1 {
            return bad("usable range");
        }
        let (count, size) = (self.entry_count(), self.entry_size());
        if count == 0 || count > MAX_ENTRIES {
            return bad("entry count");
        }
        if size != 128 && size != 256 && size != 512 {
            return Err(Error::EntrySize(size));
        }
        let bytes = u64::from(count) * u64::from(size);
        if bytes > MAX_ARRAY_BYTES {
            return bad("array over 64 KiB");
        }
        let reserved = bytes.div_ceil(SECTOR as u64).max(RESERVED_ARRAY_SECTORS);
        let at = self.entry_lba();
        let placed = if backup {
            at > last && at < capacity - 1 && reserved <= capacity - 1 - at
        } else {
            at >= 2 && at < first && reserved <= first - at
        };
        if !placed {
            return bad("array placement");
        }
        Ok(())
    }

    fn agrees_with(&self, other: &Header) -> bool {
        self.0[40..56] == other.0[40..56] && self.0[56..72] == other.0[56..72] && self.0[80..88] == other.0[80..88]
    }
}

/// One copy: its header and the whole sectors its array occupies.
#[derive(Clone, PartialEq, Eq)]
pub struct Copy {
    pub header_lba: u64,
    pub header: Header,
    pub array: Vec<u8>,
}

impl Copy {
    fn read<D: Device>(dev: &mut D, header_lba: u64, backup: bool) -> Result<Copy, Error> {
        let mut header = Header([0; SECTOR]);
        dev.read_sectors(header_lba, &mut header.0)?;
        header.check(dev.sector_count(), backup)?;
        let mut array = vec![0; array_sectors(&header) * SECTOR];
        dev.read_sectors(header.entry_lba(), &mut array)?;
        Ok(Copy {
            header_lba,
            header,
            array,
        })
    }

    pub fn declared(&self) -> &[u8] {
        let bytes = self.header.entry_count() as usize * self.header.entry_size() as usize;
        &self.array[..bytes]
    }

    pub fn consistent(&self) -> bool {
        crc32fast::hash(self.declared()) == self.header.array_crc()
    }

    pub fn entry_at(&self, index: u32) -> usize {
        index as usize * self.header.entry_size() as usize
    }

    pub fn entry(&self, index: u32) -> Entry {
        let at = self.entry_at(index);
        Entry(self.array[at..at + ENTRY_BYTES].try_into().unwrap())
    }

    pub fn set_entry(&mut self, index: u32, entry: &Entry) {
        let at = self.entry_at(index);
        self.array[at..at + ENTRY_BYTES].copy_from_slice(&entry.0);
    }
}

pub fn array_sectors(header: &Header) -> usize {
    (header.entry_count() as usize * header.entry_size() as usize).div_ceil(SECTOR)
}

/// Both copies as read. The kernel's root rule reads only the primary array, so that is the table this crate takes
/// as the card's; the backup may differ from it only where `switch_card` writes (an entry's type and name) and only
/// while one of the two copies still matches its own header's CRC.
#[derive(Clone)]
pub struct Tables {
    pub primary: Copy,
    pub backup: Copy,
}

impl Tables {
    pub fn read<D: Device>(dev: &mut D) -> Result<Tables, Error> {
        let capacity = dev.sector_count();
        let primary = Copy::read(dev, 1, false)?;
        let backup = Copy::read(dev, capacity - 1, true)?;
        if !primary.header.agrees_with(&backup.header) {
            return Err(Error::CopiesDiffer("the two headers describe different tables"));
        }
        let tables = Tables { primary, backup };
        if !tables.clean() {
            tables.check_torn()?;
        }
        Ok(tables)
    }

    pub fn count(&self) -> u32 {
        self.primary.header.entry_count()
    }

    pub fn clean(&self) -> bool {
        self.primary.consistent()
            && self.backup.consistent()
            && self.primary.header.array_crc() == self.backup.header.array_crc()
            && self.primary.declared() == self.backup.declared()
    }

    fn check_torn(&self) -> Result<(), Error> {
        if !self.primary.consistent() && !self.backup.consistent() {
            return Err(Error::CopiesDiffer("neither array matches its header's CRC"));
        }
        let size = self.primary.header.entry_size() as usize;
        let differs_outside_type_and_name = self
            .primary
            .declared()
            .chunks(size)
            .zip(self.backup.declared().chunks(size))
            .any(|(p, b)| p[16..56] != b[16..56] || p[ENTRY_BYTES..] != b[ENTRY_BYTES..]);
        if differs_outside_type_and_name {
            return Err(Error::CopiesDiffer(
                "the arrays differ in more than an entry's type and name",
            ));
        }
        Ok(())
    }
}
