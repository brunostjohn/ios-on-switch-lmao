// SPDX-License-Identifier: GPL-3.0-or-later
//! The FAT32 boot sector fields a shrink reads or writes (Microsoft's FAT specification, section 3).

use switch_card::SECTOR;

use crate::error::{Error, Result};

pub const TOT_SEC32: u16 = 32;
pub const ROOT_CLUS: u16 = 44;
pub const FSI_FREE_COUNT: u16 = 488;
pub const FSI_NXT_FREE: u16 = 492;
pub const MIN_CLUSTERS: u64 = 65526;
pub const MAX_CLUSTERS: u64 = 0x0fff_ffee;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bpb {
    pub cluster_sectors: u64,
    pub reserved: u64,
    pub fats: u64,
    pub fat_sectors: u64,
    pub total: u64,
    pub root: u32,
    /// Sector numbers inside the volume; 0 when absent.
    pub fs_info: u64,
    pub backup_boot: u64,
    pub boot_sig: u8,
    pub serial: u32,
    pub boot_crc: u32,
}

fn u16_at(b: &[u8], at: usize) -> u64 {
    u64::from(u16::from_le_bytes([b[at], b[at + 1]]))
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

/// CRC-32 of a FAT32 boot sector with the two fields a shrink may update (`TotSec32`, `RootClus`) zeroed.
pub fn boot_crc(sector: &[u8; SECTOR]) -> u32 {
    let mut s = *sector;
    s[32..36].fill(0);
    s[44..48].fill(0);
    crc32fast::hash(&s)
}

impl Bpb {
    pub fn parse(b: &[u8; SECTOR], partition_sectors: u64) -> Result<Bpb> {
        let bad = |why: &str| Err(Error::Volume(format!("FAT32 boot sector: {why}")));
        if b[510] != 0x55 || b[511] != 0xaa {
            return bad("no 0x55AA signature");
        }
        if u16_at(b, 11) != SECTOR as u64 {
            return bad("sectors are not 512 bytes");
        }
        let cluster_sectors = u64::from(b[13]);
        if !cluster_sectors.is_power_of_two() || cluster_sectors > 128 {
            return bad("sectors per cluster");
        }
        if u16_at(b, 17) != 0 || u16_at(b, 19) != 0 || u16_at(b, 22) != 0 {
            return bad("FAT12/16 fields are set: not FAT32");
        }
        let ext_flags = u16_at(b, 40);
        if ext_flags & 0xff70 != 0 {
            return bad("reserved ExtFlags bits are set");
        }
        if ext_flags & 0x80 != 0 {
            return bad("FAT mirroring is off");
        }
        if u16_at(b, 42) != 0 {
            return bad("unsupported FAT32 version");
        }
        let opt_sector = |at: usize| match u16_at(b, at) {
            0 | 0xffff => 0,
            v => v,
        };
        let bpb = Bpb {
            cluster_sectors,
            reserved: u16_at(b, 14),
            fats: u64::from(b[16]),
            fat_sectors: u64::from(u32_at(b, 36)),
            total: u64::from(u32_at(b, 32)),
            root: u32_at(b, 44),
            fs_info: opt_sector(48),
            backup_boot: opt_sector(50),
            boot_sig: b[66],
            serial: u32_at(b, 67),
            boot_crc: boot_crc(b),
        };
        if bpb.reserved == 0 || !(1..=2).contains(&bpb.fats) || bpb.fat_sectors == 0 {
            return bad("reserved sectors, FAT count or FAT size");
        }
        if bpb.total > partition_sectors || bpb.data_start() >= bpb.total {
            return bad("the volume is larger than its partition");
        }
        if bpb.clusters() < 65525
            || bpb.clusters() > MAX_CLUSTERS
            || bpb.fat_sectors * (SECTOR as u64 / 4) < bpb.clusters() + 2
        {
            return bad("cluster count or FAT size");
        }
        if bpb.root < 2 || u64::from(bpb.root) >= bpb.clusters() + 2 {
            return bad("root cluster");
        }
        if bpb.backup_boot >= bpb.reserved || bpb.fs_info >= bpb.reserved {
            return bad("backup boot sector or FSInfo outside the reserved area");
        }
        if bpb.backup_boot != 0 && bpb.backup_boot == bpb.fs_info {
            return bad("backup boot sector overlaps FSInfo");
        }
        if b[65] & 0x03 != 0 {
            return Err(Error::Damaged("the volume is marked dirty".into()));
        }
        Ok(bpb)
    }

    pub fn data_start(&self) -> u64 {
        self.reserved + self.fats * self.fat_sectors
    }

    pub fn clusters(&self) -> u64 {
        self.clusters_in(self.total)
    }

    pub fn clusters_in(&self, total: u64) -> u64 {
        total.saturating_sub(self.data_start()) / self.cluster_sectors
    }

    /// The volume's length once its partition is `partition` sectors: a volume already shorter keeps its length.
    pub fn shrunk_total(&self, partition: u64) -> u64 {
        self.total.min(partition)
    }
}

/// FSInfo's lead, structure and trail signatures.
pub fn is_fs_info(s: &[u8; SECTOR]) -> bool {
    u32_at(s, 0) == 0x4161_5252 && u32_at(s, 484) == 0x6141_7272 && u32_at(s, 508) == 0xaa55_0000
}
