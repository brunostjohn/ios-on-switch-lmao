// SPDX-License-Identifier: GPL-3.0-or-later
//! Every file and directory on a FAT32 volume by path, each file's bytes hashed, read through one chosen FAT copy.
//! Written apart from the crate's own walk, so the two don't share a mistake.

use std::collections::BTreeMap;

use sha2::{Digest, Sha256};
use switch_card::Device;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    pub dir: bool,
    pub size: u64,
    pub sha256: [u8; 32],
}

pub type Listing = BTreeMap<String, Item>;

struct Volume {
    first: u64,
    spc: u64,
    data: u64,
    fat: Vec<u32>,
    root: u32,
}

impl Volume {
    fn read_cluster<D: Device>(&self, dev: &mut D, c: u32) -> Vec<u8> {
        let mut buf = vec![0u8; self.spc as usize * 512];
        dev.read_sectors(self.first + self.data + u64::from(c - 2) * self.spc, &mut buf)
            .unwrap();
        buf
    }

    fn chain(&self, first: u32) -> Vec<u32> {
        let mut out = vec![];
        let mut c = first;
        while (2..0x0fff_fff7).contains(&c) {
            assert!(out.len() < self.fat.len(), "a loop in the chain from {first}");
            out.push(c);
            c = self.fat[c as usize];
        }
        assert!(c >= 0x0fff_fff8, "the chain from {first} ends in {c:#x}");
        out
    }
}

/// The volume at `first`, read through FAT copy `copy`.
pub fn list<D: Device>(dev: &mut D, first: u64, copy: u64) -> Listing {
    let mut b = [0u8; 512];
    dev.read_sectors(first, &mut b).unwrap();
    let u16_at = |at: usize| u64::from(u16::from_le_bytes([b[at], b[at + 1]]));
    let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    let (reserved, fats, fat_sectors) = (u16_at(14), u64::from(b[16]), u64::from(u32_at(36)));
    let data = reserved + fats * fat_sectors;
    let clusters = (u64::from(u32_at(32)) - data) / u64::from(b[13]);
    let mut raw = vec![0u8; ((clusters + 2) * 4).div_ceil(512) as usize * 512];
    dev.read_sectors(first + reserved + copy * fat_sectors, &mut raw)
        .unwrap();
    let fat = raw
        .chunks(4)
        .take(clusters as usize + 2)
        .map(|e| u32::from_le_bytes(e.try_into().unwrap()) & 0x0fff_ffff)
        .collect();
    let vol = Volume {
        first,
        spc: u64::from(b[13]),
        data,
        fat,
        root: u32_at(44),
    };
    let mut out = Listing::new();
    let root = vol.root;
    walk(dev, &vol, root, "", &mut out);
    out
}

fn walk<D: Device>(dev: &mut D, vol: &Volume, dir: u32, path: &str, out: &mut Listing) {
    let mut bytes = Vec::new();
    for c in vol.chain(dir) {
        bytes.extend(vol.read_cluster(dev, c));
    }
    let mut long: Vec<u16> = Vec::new();
    for e in bytes.chunks(32) {
        if e[0] == 0 {
            break;
        }
        if e[0] == 0xe5 {
            long.clear();
            continue;
        }
        if e[11] & 0x3f == 0x0f {
            if e[0] & 0x40 != 0 {
                long.clear();
            }
            let units: Vec<u16> = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
                .iter()
                .map(|&p| u16::from_le_bytes([e[p], e[p + 1]]))
                .take_while(|&u| u != 0 && u != 0xffff)
                .collect();
            long.splice(0..0, units);
            continue;
        }
        if e[11] & 0x08 != 0 || e[0] == b'.' {
            long.clear();
            continue;
        }
        let name = if long.is_empty() {
            String::from_utf8_lossy(&e[..11]).trim().to_string()
        } else {
            String::from_utf16_lossy(&long)
        };
        long.clear();
        let full = format!("{path}/{name}");
        let first = u32::from(u16::from_le_bytes([e[20], e[21]])) << 16 | u32::from(u16::from_le_bytes([e[26], e[27]]));
        let size = u64::from(u32::from_le_bytes(e[28..32].try_into().unwrap()));
        if e[11] & 0x10 != 0 {
            out.insert(
                full.clone(),
                Item {
                    dir: true,
                    size: 0,
                    sha256: [0; 32],
                },
            );
            walk(dev, vol, first, &full, out);
            continue;
        }
        let mut hash = Sha256::new();
        let mut left = size;
        if first != 0 {
            for c in vol.chain(first) {
                let data = vol.read_cluster(dev, c);
                let n = left.min(data.len() as u64) as usize;
                hash.update(&data[..n]);
                left -= n as u64;
            }
        }
        assert_eq!(left, 0, "{full}: its chain is shorter than its size");
        out.insert(
            full,
            Item {
                dir: false,
                size,
                sha256: hash.finalize().into(),
            },
        );
    }
}
