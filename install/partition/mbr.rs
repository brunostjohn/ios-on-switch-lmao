// SPDX-License-Identifier: GPL-3.0-or-later
//! Cards hekate partitioned without Android carry only an MBR: slot 0 the FAT, then an L4T (0x83) partition and up to
//! two emuMMC (0xE0) ones. Such a card gets a GPT, and the hybrid MBR hekate itself writes beside one: slot 0 the
//! FAT, the emuMMC slots after it in order, then the protective entry (L4T lives in the GPT only, as on hekate's
//! Android layouts).

use switch_card::SECTOR;

use crate::error::{Error, Result};

pub const FAT_TYPES: [u8; 3] = [0x0b, 0x0c, 0x07];
pub const LINUX: u8 = 0x83;
pub const EMUMMC: u8 = 0xe0;
pub const PROTECTIVE: u8 = 0xee;
/// A new GPT's arrays: 32 sectors after LBA 1, and 32 before the backup header.
pub const GPT_RESERVED: u64 = 34;

/// Last usable LBA (inclusive) of a new GPT on a card of `capacity` sectors.
pub fn last_usable(capacity: u64) -> u64 {
    capacity - GPT_RESERVED
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slot {
    pub index: usize,
    pub kind: u8,
    pub first: u64,
    pub sectors: u64,
}

impl Slot {
    pub fn end(&self) -> u64 {
        self.first + self.sectors
    }
}

fn raw(mbr: &[u8; SECTOR], index: usize) -> &[u8] {
    &mbr[446 + 16 * index..462 + 16 * index]
}

pub fn slot(mbr: &[u8; SECTOR], index: usize) -> Slot {
    let p = raw(mbr, index);
    Slot {
        index,
        kind: p[4],
        first: u64::from(u32::from_le_bytes(p[8..12].try_into().unwrap())),
        sectors: u64::from(u32::from_le_bytes(p[12..16].try_into().unwrap())),
    }
}

/// The used slots of an MBR-only card, slot 0 first, each checked the way the kernel's `validMbr` checks them and
/// to leave room for a GPT at both ends of a card of `capacity` sectors once the FAT is cut.
pub fn parts(mbr: &[u8; SECTOR], capacity: u64) -> Result<Vec<Slot>> {
    if mbr[510] != 0x55 || mbr[511] != 0xaa {
        return Err(Error::Mbr("no 0x55AA signature"));
    }
    let mut used: Vec<Slot> = Vec::new();
    let mut saw_empty = false;
    for i in 0..4 {
        let p = raw(mbr, i);
        let s = slot(mbr, i);
        if p[0] != 0 && p[0] != 0x80 {
            return Err(Error::Mbr("a boot flag other than 0 or 0x80"));
        }
        if s.kind == 0 {
            if p.iter().any(|&b| b != 0) {
                return Err(Error::Mbr("an empty slot that isn't zero"));
            }
            saw_empty = true;
            continue;
        }
        if saw_empty {
            return Err(Error::Mbr("non-contiguous MBR partition slots"));
        }
        let allowed = if i == 0 {
            FAT_TYPES.contains(&s.kind)
        } else {
            [LINUX, EMUMMC].contains(&s.kind)
        };
        if !allowed {
            return Err(Error::Mbr(
                "partitions other than the FAT in slot 0, L4T and emuMMC: not a card hekate partitioned",
            ));
        }
        // hekate's f_mkfs runs a lone FAT to the card's last sector. The FAT's tail is cut away before the backup GPT
        // goes there; any other partition in either GPT area stays in the way.
        let end_limit = if i == 0 { capacity } else { last_usable(capacity) + 1 };
        if s.sectors == 0 || s.first < GPT_RESERVED || s.end() > end_limit {
            return Err(Error::Mbr("a partition where the new GPT has to go"));
        }
        if used.iter().any(|u| s.first < u.end() && u.first < s.end()) {
            return Err(Error::Mbr("overlapping partitions"));
        }
        used.push(s);
    }
    if used.first().is_none_or(|s| s.index != 0) {
        return Err(Error::Mbr("slot 0 isn't the FAT"));
    }
    Ok(used)
}

/// The hybrid MBR for the card once its FAT is `fat_sectors` long. Everything outside the partition slots stays.
pub fn hybrid(mbr: &[u8; SECTOR], fat_sectors: u64, capacity: u64) -> Result<[u8; SECTOR]> {
    let mut out = *mbr;
    out[446..510].fill(0);
    let mut next = 0;
    let mut put = |out: &mut [u8; SECTOR], entry: &[u8]| -> Result<()> {
        if next == 4 {
            return Err(Error::Mbr("no MBR slot left for the GPT's protective entry"));
        }
        out[446 + 16 * next..462 + 16 * next].copy_from_slice(entry);
        next += 1;
        Ok(())
    };
    let mut fat: [u8; 16] = raw(mbr, 0).try_into().unwrap();
    fat[12..16].copy_from_slice(&(fat_sectors as u32).to_le_bytes());
    put(&mut out, &fat)?;
    for i in 1..4 {
        if slot(mbr, i).kind == EMUMMC {
            put(&mut out, raw(mbr, i))?;
        }
    }
    let mut protective = [0u8; 16];
    protective[4] = PROTECTIVE;
    protective[8..12].copy_from_slice(&1u32.to_le_bytes());
    protective[12..16].copy_from_slice(&((capacity - 1).min(u64::from(u32::MAX)) as u32).to_le_bytes());
    put(&mut out, &protective)?;
    Ok(out)
}

/// The same MBR with slot `index` `sectors` long and nothing else changed.
pub fn with_length(mbr: &[u8; SECTOR], index: usize, sectors: u64) -> [u8; SECTOR] {
    let mut out = *mbr;
    let at = 446 + 16 * index + 12;
    out[at..at + 4].copy_from_slice(&(sectors as u32).to_le_bytes());
    out
}
