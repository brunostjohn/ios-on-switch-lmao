// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{File, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::path::Path;

use hadris_fat::{FatVolume, FatVolumeWriteExt};
use switch_card::{BASIC_DATA_TYPE, ENTRY_BYTES, Entry, SECTOR, StreamDevice};
use switch_sd::VolumeStream;

pub const FAT_FIRST_LBA: u64 = 32_768;

pub fn make_gpt_entry(type_guid: [u8; 16], tag: u8, name: &str, first: u64, sectors: u64) -> Entry {
    let mut e = Entry([0; ENTRY_BYTES]);
    e.0[16..32].copy_from_slice(&[
        tag, 0x10, 0x20, 0x30, 0x40, 0x50, 0x4a, 0x11, 0x9f, 0x01, 0x53, 0x77, 0x69, 0x74, 0x63, 0x68,
    ]);
    e.0[32..40].copy_from_slice(&first.to_le_bytes());
    e.0[40..48].copy_from_slice(&(first + sectors - 1).to_le_bytes());
    e.retyped(type_guid, name)
}

pub fn create_gpt_card_file(path: &Path, total_sectors: u64, entries: &[(u32, Entry)]) {
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(path)
        .unwrap();
    file.set_len(total_sectors * SECTOR as u64).unwrap();

    let mut mbr = [0u8; SECTOR];
    if let Some((_, fat)) = entries
        .iter()
        .find(|(i, e)| *i == 0 && e.type_guid() == BASIC_DATA_TYPE)
    {
        write_mbr_slot(&mut mbr, 0, 0x0c, fat.first(), fat.sectors());
    }
    write_mbr_slot(&mut mbr, 1, 0xee, 1, total_sectors - 1);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    write_sector(&mut file, 0, &mbr);

    let count = 128u32;
    let entry_size = ENTRY_BYTES as u32;
    let array_sectors = (u64::from(count) * u64::from(entry_size)).div_ceil(SECTOR as u64);
    let mut array = vec![0u8; array_sectors as usize * SECTOR];
    for (idx, e) in entries {
        array[*idx as usize * ENTRY_BYTES..][..ENTRY_BYTES].copy_from_slice(&e.0);
    }
    let array_crc = crc32fast::hash(&array[..count as usize * entry_size as usize]);

    let make_header = |self_lba: u64, alt_lba: u64, entry_lba: u64| -> [u8; SECTOR] {
        let mut h = [0u8; SECTOR];
        h[..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&self_lba.to_le_bytes());
        h[32..40].copy_from_slice(&alt_lba.to_le_bytes());
        h[40..48].copy_from_slice(&(2 + array_sectors).to_le_bytes());
        h[48..56].copy_from_slice(&(total_sectors - 2 - array_sectors).to_le_bytes());
        h[56..72].copy_from_slice(b"SwitchEngineTest");
        h[72..80].copy_from_slice(&entry_lba.to_le_bytes());
        h[80..84].copy_from_slice(&count.to_le_bytes());
        h[84..88].copy_from_slice(&entry_size.to_le_bytes());
        h[88..92].copy_from_slice(&array_crc.to_le_bytes());
        let hcrc = crc32fast::hash(&h[..92]);
        h[16..20].copy_from_slice(&hcrc.to_le_bytes());
        h
    };

    let primary_hdr = make_header(1, total_sectors - 1, 2);
    let backup_entry_lba = total_sectors - 1 - array_sectors;
    let backup_hdr = make_header(total_sectors - 1, 1, backup_entry_lba);

    write_sector(&mut file, 1, &primary_hdr);
    file.seek(SeekFrom::Start(2 * SECTOR as u64)).unwrap();
    file.write_all(&array).unwrap();
    file.seek(SeekFrom::Start(backup_entry_lba * SECTOR as u64)).unwrap();
    file.write_all(&array).unwrap();
    write_sector(&mut file, total_sectors - 1, &backup_hdr);
    file.sync_data().unwrap();
}

pub fn format_fat32_region(path: &Path, first_lba: u64, total_sectors: u64, sectors_per_cluster: u8) {
    let mut file = OpenOptions::new().read(true).write(true).open(path).unwrap();
    let reserved: u16 = 32;
    let num_fats: u8 = 2;
    let spc = u64::from(sectors_per_cluster);
    let tot32 = u32::try_from(total_sectors).expect("FAT32 sectors fit u32");

    let approx_clusters = (total_sectors - u64::from(reserved)) / spc;
    let fat_sz = ((approx_clusters + 2) * 4).div_ceil(SECTOR as u64) as u32;
    let data_sectors = total_sectors - u64::from(reserved) - u64::from(num_fats) * u64::from(fat_sz);
    let cluster_count = (data_sectors / spc) as u32;
    assert!(
        cluster_count >= 65_526,
        "FAT32 needs >= 65526 clusters, got {cluster_count}"
    );

    let mut boot = [0u8; SECTOR];
    boot[0..3].copy_from_slice(&[0xeb, 0x58, 0x90]);
    boot[3..11].copy_from_slice(b"MSWIN4.1");
    boot[11..13].copy_from_slice(&(SECTOR as u16).to_le_bytes());
    boot[13] = sectors_per_cluster;
    boot[14..16].copy_from_slice(&reserved.to_le_bytes());
    boot[16] = num_fats;
    boot[21] = 0xf8;
    boot[24..26].copy_from_slice(&63u16.to_le_bytes());
    boot[26..28].copy_from_slice(&255u16.to_le_bytes());
    boot[28..32].copy_from_slice(&(first_lba as u32).to_le_bytes());
    boot[32..36].copy_from_slice(&tot32.to_le_bytes());
    boot[36..40].copy_from_slice(&fat_sz.to_le_bytes());
    boot[44..48].copy_from_slice(&2u32.to_le_bytes());
    boot[48..50].copy_from_slice(&1u16.to_le_bytes());
    boot[50..52].copy_from_slice(&6u16.to_le_bytes());
    boot[64] = 0x80;
    boot[66] = 0x29;
    boot[67..71].copy_from_slice(&0x5357_4954u32.to_le_bytes());
    boot[71..82].copy_from_slice(b"SWITCH SD  ");
    boot[82..90].copy_from_slice(b"FAT32   ");
    boot[510] = 0x55;
    boot[511] = 0xaa;

    let mut fsi = [0u8; SECTOR];
    fsi[0..4].copy_from_slice(&0x4161_5252u32.to_le_bytes());
    fsi[484..488].copy_from_slice(&0x6141_7272u32.to_le_bytes());
    fsi[488..492].copy_from_slice(&(cluster_count - 1).to_le_bytes());
    fsi[492..496].copy_from_slice(&3u32.to_le_bytes());
    fsi[510] = 0x55;
    fsi[511] = 0xaa;

    write_sector(&mut file, first_lba, &boot);
    write_sector(&mut file, first_lba + 1, &fsi);
    write_sector(&mut file, first_lba + 6, &boot);
    write_sector(&mut file, first_lba + 7, &fsi);

    let mut fat0 = [0u8; SECTOR];
    fat0[0..4].copy_from_slice(&0x0fff_fff8u32.to_le_bytes());
    fat0[4..8].copy_from_slice(&0x0fff_ffffu32.to_le_bytes());
    fat0[8..12].copy_from_slice(&0x0fff_ffffu32.to_le_bytes());
    let fat1_lba = first_lba + u64::from(reserved);
    let fat2_lba = fat1_lba + u64::from(fat_sz);
    write_sector(&mut file, fat1_lba, &fat0);
    write_sector(&mut file, fat2_lba, &fat0);

    let data_lba = fat2_lba + u64::from(fat_sz);
    let root_zeros = vec![0u8; spc as usize * SECTOR];
    file.seek(SeekFrom::Start(data_lba * SECTOR as u64)).unwrap();
    file.write_all(&root_zeros).unwrap();
    file.sync_data().unwrap();
}

pub fn write_fat_file(path: &Path, first_lba: u64, sectors: u64, rel_path: &str, data: &[u8]) {
    let file = OpenOptions::new().read(true).write(true).open(path).unwrap();
    let mut dev = StreamDevice::file(file, SECTOR as u32).unwrap();
    let stream = VolumeStream::new(&mut dev, first_lba, sectors).unwrap();
    let vol = FatVolume::open(stream).unwrap();
    let (parent_path, name) = rel_path.rsplit_once('/').unwrap_or(("", rel_path));
    let mut dir = vol.root_dir();
    for part in parent_path.split('/').filter(|p| !p.is_empty()) {
        dir = match dir.find(part).unwrap() {
            Some(_) => dir.open_dir(part).unwrap(),
            None => vol.create_dir(&dir, part).unwrap(),
        };
    }
    let entry = vol.create_file(&dir, name).unwrap();
    let mut writer = vol.write_file(&entry).unwrap();
    let mut rest = data;
    while !rest.is_empty() {
        rest = &rest[writer.write(rest).unwrap()..];
    }
    writer.finish().unwrap();
    vol.sync().unwrap();
}

pub fn read_fat_file(path: &Path, first_lba: u64, sectors: u64, rel_path: &str) -> Option<Vec<u8>> {
    let file = File::open(path).unwrap();
    let mut dev = StreamDevice::file(file, SECTOR as u32).unwrap();
    let stream = VolumeStream::new(&mut dev, first_lba, sectors).unwrap();
    let vol = FatVolume::open(stream).unwrap();
    let mut reader = vol.open_file_path(rel_path).ok()?;
    Some(reader.read_to_vec().unwrap())
}

fn write_mbr_slot(mbr: &mut [u8; SECTOR], slot: usize, kind: u8, first: u64, sectors: u64) {
    let p = &mut mbr[446 + 16 * slot..462 + 16 * slot];
    p.fill(0);
    p[4] = kind;
    p[8..12].copy_from_slice(&(first as u32).to_le_bytes());
    p[12..16].copy_from_slice(&(sectors.min(u64::from(u32::MAX)) as u32).to_le_bytes());
}

fn write_sector(file: &mut File, lba: u64, data: &[u8; SECTOR]) {
    file.seek(SeekFrom::Start(lba * SECTOR as u64)).unwrap();
    file.write_all(data).unwrap();
}
