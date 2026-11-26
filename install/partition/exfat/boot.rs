// SPDX-License-Identifier: GPL-3.0-or-later
//! The exFAT boot region fields a shrink reads or writes (Microsoft's exFAT specification, section 3).

use switch_card::SECTOR;

use crate::error::{Error, Result};

pub const JUMP_BOOT: [u8; 3] = [0xeb, 0x76, 0x90];
pub const FS_NAME: &[u8; 8] = b"EXFAT   ";
pub const REGION_SECTORS: u64 = 12;
pub const CHECKSUM_SECTOR: u64 = 11;
pub const BACKUP_OFFSET: u64 = 12;
pub const MIN_VOLUME_SECTORS: u64 = 1 << 11;
pub const MAX_CLUSTERS: u64 = 0xffff_fff5;

pub const VOLUME_LENGTH: usize = 72;
pub const CLUSTER_COUNT: usize = 92;
pub const VOLUME_FLAGS: usize = 106;
pub const PERCENT_IN_USE: usize = 112;

const FLAG_ACTIVE_FAT: u16 = 0x0001;
const FLAG_VOLUME_DIRTY: u16 = 0x0002;
const FLAG_MEDIA_FAILURE: u16 = 0x0004;
const FLAG_RESERVED_MASK: u16 = 0xfff0;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Vbr {
    pub volume_length: u64,
    pub fat_offset: u64,
    pub fat_length: u64,
    pub heap_offset: u64,
    pub cluster_count: u32,
    pub root: u32,
    pub serial: u32,
    pub spc_shift: u8,
    pub cluster_sectors: u64,
    pub percent_in_use: u8,
    pub boot_crc: u32,
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// CRC-32 of an exFAT boot sector with the fields a shrink may update masked out.
pub fn boot_crc(sector: &[u8; SECTOR]) -> u32 {
    let mut s = *sector;
    s[VOLUME_LENGTH..VOLUME_LENGTH + 8].fill(0);
    s[CLUSTER_COUNT..CLUSTER_COUNT + 4].fill(0);
    s[VOLUME_FLAGS..VOLUME_FLAGS + 2].fill(0);
    s[PERCENT_IN_USE] = 0;
    crc32fast::hash(&s)
}

/// Boot region checksum over sectors 0..11, skipping `VolumeFlags` (106..108) and `PercentInUse` (112).
pub fn region_checksum(sectors: &[u8]) -> u32 {
    assert_eq!(sectors.len(), CHECKSUM_SECTOR as usize * SECTOR);
    let mut sum = 0u32;
    for (i, &b) in sectors.iter().enumerate() {
        if i == VOLUME_FLAGS || i == VOLUME_FLAGS + 1 || i == PERCENT_IN_USE {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    sum
}

pub fn checksum_sector(sum: u32) -> [u8; SECTOR] {
    let mut out = [0u8; SECTOR];
    let bytes = sum.to_le_bytes();
    for slot in out.as_chunks_mut::<4>().0 {
        slot.copy_from_slice(&bytes);
    }
    out
}

pub fn verify_regions(main: &[u8], backup: &[u8]) -> Result<()> {
    assert_eq!(main.len(), REGION_SECTORS as usize * SECTOR);
    assert_eq!(backup.len(), REGION_SECTORS as usize * SECTOR);
    for s in 1..9usize {
        if main[s * SECTOR + 508..(s + 1) * SECTOR] != [0, 0, 0x55, 0xaa] {
            return Err(Error::Damaged("an extended boot sector signature is invalid".into()));
        }
    }
    let expected = checksum_sector(region_checksum(&main[..CHECKSUM_SECTOR as usize * SECTOR]));
    if main[CHECKSUM_SECTOR as usize * SECTOR..] != expected[..] {
        return Err(Error::Damaged("the main boot region checksum is invalid".into()));
    }
    let regions_match = main
        .iter()
        .zip(backup)
        .enumerate()
        .all(|(i, (&a, &b))| i == VOLUME_FLAGS || i == VOLUME_FLAGS + 1 || i == PERCENT_IN_USE || a == b);
    if !regions_match {
        return Err(Error::Damaged(
            "the backup boot region disagrees with the main one".into(),
        ));
    }
    Ok(())
}

impl Vbr {
    pub fn parse(b: &[u8; SECTOR], partition_sectors: u64) -> Result<Vbr> {
        let bad = |why: &str| Err(Error::Volume(format!("exFAT boot sector: {why}")));
        if b[510] != 0x55 || b[511] != 0xaa {
            return bad("no 0x55AA signature");
        }
        if b[..3] != JUMP_BOOT || &b[3..11] != FS_NAME {
            return bad("not an exFAT boot sector");
        }
        if b[11..64].iter().any(|&x| x != 0) {
            return bad("MustBeZero is non-zero");
        }
        if u16_at(b, 104) != 0x0100 {
            return bad("unsupported exFAT revision");
        }
        if b[108] != 9 {
            return bad("sectors are not 512 bytes");
        }
        let spc_shift = b[109];
        if spc_shift > 16 {
            return bad("sectors per cluster exceeds 32 MiB");
        }
        if b[110] != 1 {
            return bad("only single-FAT exFAT is supported");
        }
        let flags = u16_at(b, VOLUME_FLAGS);
        if flags & (FLAG_VOLUME_DIRTY | FLAG_MEDIA_FAILURE) != 0 {
            return Err(Error::Damaged(
                "the volume is marked dirty or reports media failure".into(),
            ));
        }
        if flags & (FLAG_ACTIVE_FAT | FLAG_RESERVED_MASK) != 0 {
            return bad("unsupported VolumeFlags");
        }
        let percent_in_use = b[PERCENT_IN_USE];
        if percent_in_use > 100 && percent_in_use != 0xff {
            return bad("invalid PercentInUse");
        }
        let volume_length = u64_at(b, VOLUME_LENGTH);
        let fat_offset = u64::from(u32_at(b, 80));
        let fat_length = u64::from(u32_at(b, 84));
        let heap_offset = u64::from(u32_at(b, 88));
        let cluster_count = u32_at(b, CLUSTER_COUNT);
        let root = u32_at(b, 96);
        let cluster_sectors = 1u64 << spc_shift;
        if volume_length < MIN_VOLUME_SECTORS || volume_length > partition_sectors {
            return bad("volume length outside its partition");
        }
        if fat_offset < REGION_SECTORS * 2 || fat_length == 0 {
            return bad("FAT offset or length");
        }
        if fat_offset.checked_add(fat_length).is_none_or(|e| e > heap_offset) || heap_offset >= volume_length {
            return bad("FAT or cluster heap placement");
        }
        let expected = ((volume_length - heap_offset) >> spc_shift).min(MAX_CLUSTERS);
        if cluster_count == 0 || u64::from(cluster_count) != expected {
            return bad("cluster count does not match volume length");
        }
        if fat_length
            .checked_mul(128)
            .is_none_or(|n| n < u64::from(cluster_count) + 2)
        {
            return bad("FAT too short for cluster count");
        }
        if root < 2 || root >= cluster_count + 2 {
            return bad("root directory cluster");
        }
        Ok(Vbr {
            volume_length,
            fat_offset,
            fat_length,
            heap_offset,
            cluster_count,
            root,
            serial: u32_at(b, 100),
            spc_shift,
            cluster_sectors,
            percent_in_use,
            boot_crc: boot_crc(b),
        })
    }

    pub fn clusters_in(&self, volume_length: u64) -> u64 {
        volume_length
            .saturating_sub(self.heap_offset)
            .checked_shr(u32::from(self.spc_shift))
            .unwrap_or(0)
            .min(MAX_CLUSTERS)
    }

    pub fn shrunk_total(&self, partition: u64) -> u64 {
        self.volume_length.min(partition)
    }

    pub fn cluster_bytes(&self) -> u64 {
        self.cluster_sectors * SECTOR as u64
    }
}
