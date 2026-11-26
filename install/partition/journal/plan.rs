// SPDX-License-Identifier: GPL-3.0-or-later
//! The end state a run works towards, kept in every journal header so a resume needs nothing else.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FsKind {
    Fat32,
    Exfat,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Fingerprint {
    pub vol_serial: u32,
    pub vol_boot_sig: u8,
    pub vol_total: u64,
    pub boot_crc: u32,
    pub table_crc: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    pub card_sectors: u64,
    pub fat_first: u64,
    pub fat_sectors: u64,
    pub fat_new_sectors: u64,
    pub entry_first: u64,
    pub entry_sectors: u64,
    pub entry_index: u32,
    pub entry_guid: [u8; 16],
    pub fs: FsKind,
    /// An MBR-only card that gets a GPT, with this disk GUID.
    pub new_gpt: Option<[u8; 16]>,
    pub fingerprint: Fingerprint,
}

impl Plan {
    pub fn entry_last(&self) -> u64 {
        self.entry_first + self.entry_sectors - 1
    }

    pub(crate) fn encode(&self, out: &mut [u8]) {
        let mut at = 0;
        let mut put = |bytes: &[u8]| {
            out[at..at + bytes.len()].copy_from_slice(bytes);
            at += bytes.len();
        };
        for v in [
            self.card_sectors,
            self.fat_first,
            self.fat_sectors,
            self.fat_new_sectors,
            self.entry_first,
            self.entry_sectors,
        ] {
            put(&v.to_le_bytes());
        }
        put(&self.entry_index.to_le_bytes());
        put(&self.entry_guid);
        put(&[
            match self.fs {
                FsKind::Fat32 => 1,
                FsKind::Exfat => 2,
            },
            u8::from(self.new_gpt.is_some()),
        ]);
        put(&self.new_gpt.unwrap_or_default());
        put(&self.fingerprint.vol_serial.to_le_bytes());
        put(&[self.fingerprint.vol_boot_sig]);
        put(&self.fingerprint.vol_total.to_le_bytes());
        put(&self.fingerprint.boot_crc.to_le_bytes());
        put(&self.fingerprint.table_crc.to_le_bytes());
    }

    pub(crate) fn decode(b: &[u8]) -> Option<Plan> {
        let u64_at = |at: usize| u64::from_le_bytes(b[at..at + 8].try_into().unwrap());
        let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
        let fs = match b[68] {
            1 => FsKind::Fat32,
            2 => FsKind::Exfat,
            _ => return None,
        };
        Some(Plan {
            card_sectors: u64_at(0),
            fat_first: u64_at(8),
            fat_sectors: u64_at(16),
            fat_new_sectors: u64_at(24),
            entry_first: u64_at(32),
            entry_sectors: u64_at(40),
            entry_index: u32_at(48),
            entry_guid: b[52..68].try_into().unwrap(),
            fs,
            new_gpt: (b[69] != 0).then(|| b[70..86].try_into().unwrap()),
            fingerprint: Fingerprint {
                vol_serial: u32_at(86),
                vol_boot_sig: b[90],
                vol_total: u64_at(91),
                boot_crc: u32_at(99),
                table_crc: u32_at(103),
            },
        })
    }
}
