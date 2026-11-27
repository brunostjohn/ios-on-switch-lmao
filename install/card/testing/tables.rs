// SPDX-License-Identifier: GPL-3.0-or-later
//! A test-only GPT writer: synthetic cards, and the owner's card re-laid out as the partitioning thread lays it out.

use switch_card::{BASIC_DATA_TYPE, ENTRY_BYTES, Entry, SECTOR};

use crate::image::SparseImage;

pub const RESERVED_ARRAY_SECTORS: u64 = 32;

pub fn entry(type_guid: [u8; 16], tag: u8, name: &str, first: u64, sectors: u64) -> Entry {
    let mut e = Entry([0; ENTRY_BYTES]);
    e.0[16..32].copy_from_slice(&[
        tag, 0xc0, 0xde, 0x5a, 0x1e, 0, 0x4a, 0x11, 0x9f, 0, 0x53, 0x77, 0x69, 0x74, 0x63, 0x68,
    ]);
    e.0[32..40].copy_from_slice(&first.to_le_bytes());
    e.0[40..48].copy_from_slice(&(first + sectors - 1).to_le_bytes());
    e.retyped(type_guid, name)
}

pub fn mbr_slot(image: &mut SparseImage, slot: usize, kind: u8, first: u64, sectors: u64) {
    let mut mbr = image.sector(0);
    let p = &mut mbr[446 + 16 * slot..462 + 16 * slot];
    p.fill(0);
    p[4] = kind;
    p[8..12].copy_from_slice(&(first as u32).to_le_bytes());
    p[12..16].copy_from_slice(&(sectors.min(u64::from(u32::MAX)) as u32).to_le_bytes());
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    image.set(0, &mbr);
}

/// A card of `sectors` with a hybrid MBR (slot 0 the FAT, slot 1 protective) and `entries` in a GPT of
/// `entry_size`-byte entries, `count` of them declared.
pub fn card(sectors: u64, entry_size: u32, count: u32, entries: &[(u32, Entry)]) -> SparseImage {
    let mut image = SparseImage::new(sectors);
    if let Some((_, fat)) = entries
        .iter()
        .find(|(i, e)| *i == 0 && e.type_guid() == BASIC_DATA_TYPE)
    {
        mbr_slot(&mut image, 0, 0x0c, fat.first(), fat.sectors());
    }
    mbr_slot(&mut image, 1, 0xee, 1, sectors - 1);
    let array_sectors = (u64::from(count) * u64::from(entry_size))
        .div_ceil(SECTOR as u64)
        .max(RESERVED_ARRAY_SECTORS);
    let mut header = [0u8; SECTOR];
    header[..8].copy_from_slice(b"EFI PART");
    header[8..12].copy_from_slice(&0x10000u32.to_le_bytes());
    header[12..16].copy_from_slice(&92u32.to_le_bytes());
    header[24..32].copy_from_slice(&1u64.to_le_bytes());
    header[32..40].copy_from_slice(&(sectors - 1).to_le_bytes());
    header[40..48].copy_from_slice(&(2 + array_sectors).to_le_bytes());
    header[48..56].copy_from_slice(&(sectors - 2 - array_sectors).to_le_bytes());
    header[56..72].copy_from_slice(b"SwitchCardTest01");
    header[72..80].copy_from_slice(&2u64.to_le_bytes());
    header[84..88].copy_from_slice(&entry_size.to_le_bytes());
    let mut backup = header;
    backup[24..32].copy_from_slice(&(sectors - 1).to_le_bytes());
    backup[32..40].copy_from_slice(&1u64.to_le_bytes());
    backup[72..80].copy_from_slice(&(sectors - 1 - array_sectors).to_le_bytes());
    image.set(1, &header);
    image.set(sectors - 1, &backup);
    rewrite(&mut image, count, |array| {
        for (i, e) in entries {
            array[*i as usize * entry_size as usize..][..ENTRY_BYTES].copy_from_slice(&e.0);
        }
    });
    image
}

/// Both arrays set from the primary one after `edit`, `count` entries declared, both headers' CRCs redone.
pub fn rewrite(image: &mut SparseImage, count: u32, edit: impl FnOnce(&mut Vec<u8>)) {
    let primary = image.sector(1);
    let at = |h: &[u8; SECTOR], o: usize| u64::from_le_bytes(h[o..o + 8].try_into().unwrap());
    let size = u32::from_le_bytes(primary[84..88].try_into().unwrap()) as usize;
    let sectors = (count as usize * size).div_ceil(SECTOR) as u64;
    let mut array = image.read_bytes(at(&primary, 72), sectors);
    edit(&mut array);
    let crc = crc32fast::hash(&array[..count as usize * size]);
    for lba in [1, image.sectors - 1] {
        let mut h = image.sector(lba);
        image.set(at(&h, 72), &array);
        h[80..84].copy_from_slice(&count.to_le_bytes());
        h[88..92].copy_from_slice(&crc.to_le_bytes());
        h[16..20].fill(0);
        let header_crc = crc32fast::hash(&h[..92]);
        h[16..20].copy_from_slice(&header_crc.to_le_bytes());
        image.set(lba, &h);
    }
}

pub fn set_entry(array: &mut [u8], index: u32, e: &Entry) {
    array[index as usize * ENTRY_BYTES..][..ENTRY_BYTES].copy_from_slice(&e.0);
}

pub fn get_entry(array: &[u8], index: u32) -> Entry {
    Entry(array[index as usize * ENTRY_BYTES..][..ENTRY_BYTES].try_into().unwrap())
}
