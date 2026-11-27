// SPDX-License-Identifier: GPL-3.0-or-later
//! Independent exFAT reader for tests: walks the tree apart from `switch_partition`'s own walk, hashing every file
//! through `NoFatChain` or FAT chains and verifying boot checksums, entry-set checksums, and bitmap consistency.

use std::collections::BTreeSet;

use sha2::{Digest, Sha256};
use switch_card::Device;

use crate::exfat::{decode_upcase, entry_set_checksum};
use crate::reader::{Item, Listing};

const EOC: u32 = 0xffff_ffff;

struct Volume {
    first: u64,
    spc: u64,
    heap: u64,
    clusters: u32,
    fat: Vec<u32>,
    bitmap: Vec<u8>,
    upcase: Vec<u16>,
}

fn region_checksum(sectors: &[u8]) -> u32 {
    let mut sum = 0u32;
    for (i, &b) in sectors.iter().enumerate() {
        if i == 106 || i == 107 || i == 112 {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    sum
}

fn check_region(region: &[u8]) -> bool {
    let want = region_checksum(&region[..11 * 512]).to_le_bytes();
    region[11 * 512..12 * 512].as_chunks::<4>().0.iter().all(|s| s == &want)
}

impl Volume {
    fn read_cluster<D: Device>(&self, dev: &mut D, c: u32) -> Vec<u8> {
        let mut buf = vec![0u8; self.spc as usize * 512];
        dev.read_sectors(self.first + self.heap + u64::from(c - 2) * self.spc, &mut buf)
            .unwrap();
        buf
    }

    fn bit(&self, c: u32) -> bool {
        let bit = (c - 2) as usize;
        (self.bitmap[bit / 8] >> (bit % 8)) & 1 != 0
    }

    fn chain(&self, first: u32) -> Vec<u32> {
        let mut out = Vec::new();
        let mut c = first;
        loop {
            assert!(c >= 2 && c < self.clusters + 2, "cluster {c} out of bounds");
            assert!(out.len() < self.fat.len(), "loop in chain from {first}");
            out.push(c);
            let next = self.fat[c as usize];
            if next == EOC {
                return out;
            }
            c = next;
        }
    }

    fn name_hash(&self, units: &[u16]) -> u16 {
        let mut hash = 0u16;
        for &u in units {
            let up = self.upcase.get(u as usize).copied().unwrap_or(u);
            for b in up.to_le_bytes() {
                hash = hash.rotate_right(1).wrapping_add(u16::from(b));
            }
        }
        hash
    }
}

/// Lists and hashes every file and directory on the exFAT volume at `first`.
/// When `strict` is true, enforces matching boot regions, every entry-set checksum, `NameHash`, and zero lost clusters.
/// When `strict` is false (an un-resumed power cut), falls back to the backup boot region if main was torn mid-shrink,
/// tolerates leaked clusters, and relaxes `SetChecksum` only for entries straddling a 512-byte sector boundary.
pub fn list<D: Device>(dev: &mut D, first: u64, strict: bool) -> Listing {
    let mut regions = vec![0u8; 24 * 512];
    dev.read_sectors(first, &mut regions).unwrap();
    let (main, backup) = regions.split_at(12 * 512);
    let boot = if strict {
        assert!(check_region(main), "main boot region checksum invalid");
        let regions_match = main
            .iter()
            .zip(backup)
            .enumerate()
            .all(|(i, (&a, &b))| i == 106 || i == 107 || i == 112 || a == b);
        assert!(regions_match, "backup boot region differs from main");
        &main[..512]
    } else if check_region(main) {
        &main[..512]
    } else {
        assert!(check_region(backup), "both main and backup boot regions invalid");
        &backup[..512]
    };

    assert_eq!(&boot[3..11], b"EXFAT   ");
    let u32_at = |at: usize| u32::from_le_bytes(boot[at..at + 4].try_into().unwrap());
    let fat_off = u64::from(u32_at(80));
    let heap = u64::from(u32_at(88));
    let clusters = u32_at(92);
    let root = u32_at(96);
    let spc = 1u64 << boot[109];
    let cb = spc * 512;

    let entries = u64::from(clusters) + 2;
    let mut raw_fat = vec![0u8; (entries * 4).div_ceil(512) as usize * 512];
    dev.read_sectors(first + fat_off, &mut raw_fat).unwrap();
    let fat: Vec<u32> = raw_fat
        .as_chunks::<4>()
        .0
        .iter()
        .take(entries as usize)
        .map(|&e| u32::from_le_bytes(e))
        .collect();

    let mut vol = Volume {
        first,
        spc,
        heap,
        clusters,
        fat,
        bitmap: Vec::new(),
        upcase: Vec::new(),
    };

    let root_chain = vol.chain(root);
    let mut root_bytes = Vec::new();
    for &c in &root_chain {
        root_bytes.extend(vol.read_cluster(dev, c));
    }

    let mut bm_info = None;
    let mut up_info = None;
    for e in root_bytes.as_chunks::<32>().0 {
        if e[0] == 0 {
            break;
        }
        if e[0] == 0x81 {
            bm_info = Some((
                u32::from_le_bytes(e[20..24].try_into().unwrap()),
                u64::from_le_bytes(e[24..32].try_into().unwrap()),
            ));
        } else if e[0] == 0x82 {
            up_info = Some((
                u32::from_le_bytes(e[4..8].try_into().unwrap()),
                u32::from_le_bytes(e[20..24].try_into().unwrap()),
                u64::from_le_bytes(e[24..32].try_into().unwrap()),
            ));
        }
    }

    let (bm_first, bm_len) = bm_info.expect("missing 0x81 allocation bitmap");
    assert!(bm_len >= u64::from(clusters).div_ceil(8), "bitmap DataLength too short");
    let bm_chain = vol.chain(bm_first);
    let mut bm_bytes = Vec::new();
    for &c in &bm_chain {
        bm_bytes.extend(vol.read_cluster(dev, c));
    }
    bm_bytes.truncate(u64::from(clusters).div_ceil(8) as usize);
    vol.bitmap = bm_bytes;

    let (up_sum, up_first, up_len) = up_info.expect("missing 0x82 up-case table");
    let up_chain = vol.chain(up_first);
    let mut up_bytes = Vec::new();
    for &c in &up_chain {
        up_bytes.extend(vol.read_cluster(dev, c));
    }
    up_bytes.truncate(up_len as usize);
    if strict {
        let mut calc = 0u32;
        for &b in &up_bytes {
            calc = calc.rotate_right(1).wrapping_add(u32::from(b));
        }
        assert_eq!(calc, up_sum, "up-case table checksum mismatch");
    }
    vol.upcase = decode_upcase(&up_bytes);

    let mut owned = BTreeSet::new();
    for &c in bm_chain.iter().chain(&up_chain).chain(&root_chain) {
        assert!(vol.bit(c), "system cluster {c} not marked in bitmap");
        assert!(owned.insert(c), "system cluster {c} cross-linked");
    }

    let mut out = Listing::new();
    walk(dev, &vol, &root_chain, "", strict, cb, &mut owned, &mut out);

    if strict {
        for c in 2..clusters + 2 {
            if vol.bit(c) {
                assert!(owned.contains(&c), "lost cluster {c} marked in bitmap");
            }
        }
    }
    out
}

#[allow(clippy::too_many_arguments)]
fn walk<D: Device>(
    dev: &mut D,
    vol: &Volume,
    dir_chain: &[u32],
    path: &str,
    strict: bool,
    cb: u64,
    owned: &mut BTreeSet<u32>,
    out: &mut Listing,
) {
    let mut bytes = Vec::new();
    for &c in dir_chain {
        bytes.extend(vol.read_cluster(dev, c));
    }
    let total = bytes.len() / 32;
    let mut i = 0;
    while i < total {
        let e = &bytes[i * 32..(i + 1) * 32];
        match e[0] {
            0x00 => break,
            0x85 => {}
            _ => {
                i += 1;
                continue;
            }
        }
        let sec_count = e[1] as usize;
        let set = &bytes[i * 32..(i + 1 + sec_count) * 32];
        let straddles_sector = (i * 32) % 512 == 480;
        if strict || !straddles_sector {
            let stored = u16::from_le_bytes([set[2], set[3]]);
            assert_eq!(entry_set_checksum(set), stored, "bad SetChecksum at {path} slot {i}");
        }
        let attrs = u16::from_le_bytes([set[4], set[5]]);
        let is_dir = attrs & 0x0010 != 0;
        let stream = &set[32..64];
        assert_eq!(stream[0], 0xc0);
        let no_fat_chain = stream[1] & 0x02 != 0;
        let name_len = stream[3] as usize;
        let stored_hash = u16::from_le_bytes([stream[4], stream[5]]);
        let valid_len = u64::from_le_bytes(stream[8..16].try_into().unwrap());
        let first = u32::from_le_bytes(stream[20..24].try_into().unwrap());
        let data_len = u64::from_le_bytes(stream[24..32].try_into().unwrap());
        assert!(valid_len <= data_len);

        let mut units = Vec::with_capacity(name_len);
        for k in 0..name_len.div_ceil(15) {
            let ne = &set[(2 + k) * 32..(3 + k) * 32];
            assert_eq!(ne[0], 0xc1);
            for j in 0..15 {
                if units.len() == name_len {
                    break;
                }
                units.push(u16::from_le_bytes([ne[2 + j * 2], ne[3 + j * 2]]));
            }
        }
        if strict {
            assert_eq!(vol.name_hash(&units), stored_hash, "bad NameHash");
        }
        let name = String::from_utf16_lossy(&units);
        let full = format!("{path}/{name}");
        i += 1 + sec_count;

        let need = data_len.div_ceil(cb) as usize;
        let chain = if need == 0 {
            assert_eq!(first, 0);
            Vec::new()
        } else if no_fat_chain {
            (first..first + need as u32).collect()
        } else {
            let c = vol.chain(first);
            assert_eq!(c.len(), need, "{full}: chain length mismatch");
            c
        };
        for &c in &chain {
            assert!(vol.bit(c), "{full}: cluster {c} free in bitmap");
            assert!(owned.insert(c), "{full}: cluster {c} cross-linked");
        }

        if is_dir {
            out.insert(
                full.clone(),
                Item {
                    dir: true,
                    size: 0,
                    sha256: [0; 32],
                },
            );
            walk(dev, vol, &chain, &full, strict, cb, owned, out);
        } else {
            let mut hash = Sha256::new();
            let mut left = valid_len;
            for &c in &chain {
                let data = vol.read_cluster(dev, c);
                let n = left.min(data.len() as u64) as usize;
                hash.update(&data[..n]);
                left -= n as u64;
            }
            if data_len > valid_len {
                hash.update(vec![0u8; (data_len - valid_len) as usize]);
            }
            out.insert(
                full,
                Item {
                    dir: false,
                    size: data_len,
                    sha256: hash.finalize().into(),
                },
            );
        }
    }
}
