// SPDX-License-Identifier: GPL-3.0-or-later
//! A GPT (UEFI 2.10, section 5.3) with one Apple_APFS partition, for diskutil to grow a container in. Unprivileged
//! users can't partition an attached image with gpt(8), so the table is written into the image file before attaching.

use std::fs::OpenOptions;
use std::io::{Seek, SeekFrom, Write};
use std::path::Path;

const SECTOR: u64 = 512;
const ENTRIES: u64 = 128;
const ENTRY_BYTES: usize = 128;
const TABLE_SECTORS: u64 = ENTRIES * ENTRY_BYTES as u64 / SECTOR;
/// 7C3457EF-0000-11AA-AA11-00306543ECAC, mixed-endian as GPT stores it.
const APFS_TYPE: [u8; 16] = [
    0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC,
];
const DISK_GUID: [u8; 16] = *b"switch-sweep-dsk";
const PARTITION_GUID: [u8; 16] = *b"switch-sweep-prt";

pub fn write(image: &Path, first: u64, sectors: u64) -> std::io::Result<()> {
    let mut f = OpenOptions::new().write(true).open(image)?;
    let last = f.metadata()?.len() / SECTOR - 1;
    let mut table = vec![0u8; ENTRIES as usize * ENTRY_BYTES];
    table[..16].copy_from_slice(&APFS_TYPE);
    table[16..32].copy_from_slice(&PARTITION_GUID);
    table[32..40].copy_from_slice(&first.to_le_bytes());
    table[40..48].copy_from_slice(&(first + sectors - 1).to_le_bytes());
    for (i, unit) in "Sys".encode_utf16().enumerate() {
        table[56 + 2 * i..58 + 2 * i].copy_from_slice(&unit.to_le_bytes());
    }
    let table_crc = crc32(&table);

    let mut mbr = [0u8; SECTOR as usize];
    mbr[446..462].copy_from_slice(&protective_entry(last));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    put(&mut f, 0, &mbr)?;
    put(&mut f, 1, &header(1, last, 2, last, table_crc))?;
    put(&mut f, 2, &table)?;
    put(&mut f, last - TABLE_SECTORS, &table)?;
    put(&mut f, last, &header(last, 1, last - TABLE_SECTORS, last, table_crc))?;
    f.sync_all()
}

fn protective_entry(last: u64) -> [u8; 16] {
    let mut e = [0u8; 16];
    e[2] = 2;
    e[4] = 0xEE;
    e[5..8].copy_from_slice(&[0xFF; 3]);
    e[8..12].copy_from_slice(&1u32.to_le_bytes());
    e[12..16].copy_from_slice(&(last.min(u64::from(u32::MAX)) as u32).to_le_bytes());
    e
}

fn header(current: u64, backup: u64, entries_lba: u64, last: u64, table_crc: u32) -> [u8; SECTOR as usize] {
    let mut h = [0u8; SECTOR as usize];
    h[..8].copy_from_slice(b"EFI PART");
    h[8..12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
    h[12..16].copy_from_slice(&92u32.to_le_bytes());
    h[24..32].copy_from_slice(&current.to_le_bytes());
    h[32..40].copy_from_slice(&backup.to_le_bytes());
    h[40..48].copy_from_slice(&(2 + TABLE_SECTORS).to_le_bytes());
    h[48..56].copy_from_slice(&(last - TABLE_SECTORS - 1).to_le_bytes());
    h[56..72].copy_from_slice(&DISK_GUID);
    h[72..80].copy_from_slice(&entries_lba.to_le_bytes());
    h[80..84].copy_from_slice(&(ENTRIES as u32).to_le_bytes());
    h[84..88].copy_from_slice(&(ENTRY_BYTES as u32).to_le_bytes());
    h[88..92].copy_from_slice(&table_crc.to_le_bytes());
    let crc = crc32(&h[..92]);
    h[16..20].copy_from_slice(&crc.to_le_bytes());
    h
}

fn put(f: &mut std::fs::File, lba: u64, bytes: &[u8]) -> std::io::Result<()> {
    f.seek(SeekFrom::Start(lba * SECTOR))?;
    f.write_all(bytes)
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in bytes {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg());
        }
    }
    !c
}
