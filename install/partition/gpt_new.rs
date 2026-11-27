// SPDX-License-Identifier: GPL-3.0-or-later
//! A GPT for a card that has none, laid out as hekate lays its own out: 128-byte entries declared only as far as
//! they're used, arrays at LBA 2 and 33 sectors from the end, usable space between.

use switch_card::{ENTRY_BYTES, Entry, SECTOR};

use crate::mbr::{self, GPT_RESERVED};

/// The sectors of a new GPT in the order they go down: backup array, backup header, primary array, primary header.
pub fn sectors(capacity: u64, disk_guid: [u8; 16], entries: &[Entry]) -> Vec<(u64, [u8; SECTOR])> {
    let count = entries.len() as u32;
    let mut array = vec![0u8; (entries.len() * ENTRY_BYTES).div_ceil(SECTOR) * SECTOR];
    for (i, e) in entries.iter().enumerate() {
        array[i * ENTRY_BYTES..][..ENTRY_BYTES].copy_from_slice(&e.0);
    }
    let array_crc = crc32fast::hash(&array[..entries.len() * ENTRY_BYTES]);
    let backup_array = capacity - (GPT_RESERVED - 1);
    let header = |mine: u64, other: u64, entry_lba: u64| {
        let mut h = [0u8; SECTOR];
        h[..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&0x10000u32.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&mine.to_le_bytes());
        h[32..40].copy_from_slice(&other.to_le_bytes());
        h[40..48].copy_from_slice(&GPT_RESERVED.to_le_bytes());
        h[48..56].copy_from_slice(&mbr::last_usable(capacity).to_le_bytes());
        h[56..72].copy_from_slice(&disk_guid);
        h[72..80].copy_from_slice(&entry_lba.to_le_bytes());
        h[80..84].copy_from_slice(&count.to_le_bytes());
        h[84..88].copy_from_slice(&(ENTRY_BYTES as u32).to_le_bytes());
        h[88..92].copy_from_slice(&array_crc.to_le_bytes());
        let crc = crc32fast::hash(&h[..92]);
        h[16..20].copy_from_slice(&crc.to_le_bytes());
        h
    };
    let array_sectors = |first: u64| {
        array
            .chunks(SECTOR)
            .enumerate()
            .map(move |(i, s)| (first + i as u64, <[u8; SECTOR]>::try_from(s).unwrap()))
    };
    let mut out: Vec<(u64, [u8; SECTOR])> = array_sectors(backup_array).collect();
    out.push((capacity - 1, header(capacity - 1, 1, backup_array)));
    out.extend(array_sectors(2));
    out.push((1, header(1, capacity - 1, 2)));
    out
}

/// An entry of `type_guid` named `name` over `first..first+sectors`.
pub fn entry(type_guid: [u8; 16], unique: [u8; 16], name: &str, first: u64, sectors: u64) -> Entry {
    let mut e = Entry([0; ENTRY_BYTES]);
    e.0[16..32].copy_from_slice(&unique);
    e.0[32..40].copy_from_slice(&first.to_le_bytes());
    e.0[40..48].copy_from_slice(&(first + sectors - 1).to_le_bytes());
    e.retyped(type_guid, name)
}
