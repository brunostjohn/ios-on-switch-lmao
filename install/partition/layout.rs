// SPDX-License-Identifier: GPL-3.0-or-later
//! The card as partition mode finds it: its tables (a GPT, or hekate's MBR alone), the FAT at GPT index 0 / MBR
//! slot 0, and where a new entry can go.

use sha2::{Digest, Sha256};
use switch_card::{Card, Device, SECTOR, SETUP_NAME};

use crate::error::{Error, Result};
use crate::journal::{self, FsKind};
use crate::mbr::{self, GPT_RESERVED, Slot};
use crate::size::{ALIGN_SECTORS, align_down};

pub enum Tables {
    Gpt(Box<Card>),
    /// The used MBR slots, slot 0 (the FAT) first.
    MbrOnly(Box<[u8; SECTOR]>, Vec<Slot>),
}

pub struct Layout {
    pub capacity: u64,
    pub fat_first: u64,
    pub fat_sectors: u64,
    pub fs: FsKind,
    pub tables: Tables,
    /// One past the last sector a new entry may use.
    pub entry_end: u64,
    pub entry_index: u32,
    /// Bytes unique to this card, for the GUIDs a run makes.
    pub seed: Vec<u8>,
}

impl Layout {
    pub fn read<D: Device>(dev: &mut D) -> Result<Layout> {
        let capacity = dev.sector_count();
        let (tables, fat_first, fat_sectors, entry_end, entry_index, mut seed) = match Card::open(dev) {
            Ok(card) => {
                if card.torn() {
                    return Err(switch_card::Error::CopiesDiffer("a cut-short write is still in the tables").into());
                }
                let (first, sectors) = card.boot_volume()?;
                if let Some(i) = card.entries().iter().position(|e| e.is_used() && e.named(SETUP_NAME)) {
                    return Err(Error::AlreadyPartitioned(i as u32));
                }
                let roots = card.roots();
                if let Some(&foreign) = roots.iter().find(|&&i| card.role(i).is_none()) {
                    return Err(switch_card::Error::ForeignRoot(foreign).into());
                }
                if roots.len() > 1 {
                    return Err(switch_card::Error::Ambiguous(roots).into());
                }
                let journal_clear = card.usable().0 <= journal::FIRST
                    && journal::FIRST + journal::SECTORS - 1 <= card.usable().1
                    && !card
                        .entries()
                        .iter()
                        .any(|e| e.is_used() && overlaps_journal(e.first(), e.last() + 1));
                if !journal_clear {
                    return Err(Error::Journal("a partition or the GPT itself is there"));
                }
                let end = card.usable().1 + 1;
                let index = card
                    .entries()
                    .iter()
                    .rposition(|e| e.is_used())
                    .map_or(0, |i| i as u32 + 1);
                let new_count = card.entries().len().max(index as usize + 1) as u32;
                card.check_entry_count(new_count)?;
                let seed = card.disk_guid().to_vec();
                (Tables::Gpt(Box::new(card)), first, sectors, end, index, seed)
            }
            Err(switch_card::Error::NoGpt) => {
                let mut sector = [0u8; SECTOR];
                dev.read_sectors(0, &mut sector)?;
                let parts = mbr::parts(&sector, capacity)?;
                if parts.iter().any(|s| overlaps_journal(s.first, s.end())) {
                    return Err(Error::Journal("a partition is there"));
                }
                let mut head = vec![0u8; (GPT_RESERVED as usize - 1) * SECTOR];
                dev.read_sectors(1, &mut head)?;
                if head.iter().any(|&b| b != 0) {
                    return Err(Error::Mbr("LBA 1-33 hold data, where the new GPT goes"));
                }
                let fat = parts[0];
                mbr::hybrid(&sector, fat.sectors, capacity)?;
                let end = mbr::last_usable(capacity) + 1;
                let index = parts.len() as u32;
                (
                    Tables::MbrOnly(Box::new(sector), parts),
                    fat.first,
                    fat.sectors,
                    end,
                    index,
                    sector.to_vec(),
                )
            }
            Err(e) => return Err(e.into()),
        };
        let mut boot = [0u8; SECTOR];
        dev.read_sectors(fat_first, &mut boot)?;
        let fs = if &boot[3..11] == b"EXFAT   " {
            FsKind::Exfat
        } else {
            FsKind::Fat32
        };
        let mbr_type = match &tables {
            Tables::Gpt(card) => card.mbr()[450],
            Tables::MbrOnly(sector, _) => sector[450],
        };
        let refused_type = if fs == FsKind::Exfat { 0x0b } else { 0x07 };
        if mbr_type == refused_type {
            return Err(Error::Mbr(
                "slot 0's type is one the kernel refuses for this file system",
            ));
        }
        seed.extend_from_slice(&capacity.to_le_bytes());
        seed.extend_from_slice(&boot);
        Ok(Layout {
            capacity,
            fat_first,
            fat_sectors,
            fs,
            tables,
            entry_end,
            entry_index,
            seed,
        })
    }

    /// Where an iOS entry of `ios_sectors` goes: the FAT's new length and the entry's first sector. The entry ends on
    /// the last 16 MiB boundary inside the FAT's old extent (and the usable space), and starts `ios_sectors` before.
    pub fn carve(&self, ios_sectors: u64) -> Result<(u64, u64)> {
        let bytes = ios_sectors * SECTOR as u64;
        let refuse = |why: &str| Err(Error::Size { bytes, why: why.into() });
        if ios_sectors == 0 || !ios_sectors.is_multiple_of(ALIGN_SECTORS) {
            return refuse("not a whole number of 16 MiB");
        }
        let end = align_down(
            (self.fat_first + self.fat_sectors)
                .min(self.entry_end)
                .min(u64::from(u32::MAX) + 1),
            ALIGN_SECTORS,
        );
        let Some(first) = end.checked_sub(ios_sectors).filter(|&f| f > self.fat_first) else {
            return refuse("more than the boot volume holds");
        };
        if end - 1 > u64::from(u32::MAX) {
            return refuse("it would reach past LBA 2^32 - 1, which the kernel can't address");
        }
        Ok((first - self.fat_first, first))
    }

    pub fn guid(&self, tag: &str) -> [u8; 16] {
        derived_guid(tag, &self.seed)
    }
}

/// CRC-32 of the table sectors that stay unchanged before `Phase::Tables` (LBA 0 and LBA 1; plus LBA `capacity - 1`
/// for GPT cards, since an MBR-only FAT-to-the-end card may still hold FAT data at the last sector before Shrink).
pub fn table_crc<D: Device>(dev: &mut D, capacity: u64, mbr_only: bool) -> Result<u32> {
    let mut hasher = crc32fast::Hasher::new();
    let mut sector = [0u8; SECTOR];
    dev.read_sectors(0, &mut sector)?;
    hasher.update(&sector);
    dev.read_sectors(1, &mut sector)?;
    hasher.update(&sector);
    if !mbr_only {
        dev.read_sectors(capacity - 1, &mut sector)?;
        hasher.update(&sector);
    }
    Ok(hasher.finalize())
}

/// A version 4 GUID (GPT byte order) from SHA-256 of `tag` and `seed`: the same card and plan always get the same.
pub fn derived_guid(tag: &str, seed: &[u8]) -> [u8; 16] {
    let digest = Sha256::new()
        .chain_update(b"switch_partition\0")
        .chain_update(tag.as_bytes())
        .chain_update(seed)
        .finalize();
    let mut g: [u8; 16] = digest[..16].try_into().unwrap();
    g[7] = (g[7] & 0x0f) | 0x40;
    g[8] = (g[8] & 0x3f) | 0x80;
    g
}

fn overlaps_journal(first: u64, end: u64) -> bool {
    first < journal::FIRST + journal::SECTORS && journal::FIRST < end
}
