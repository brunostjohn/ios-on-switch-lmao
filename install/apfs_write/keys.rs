// SPDX-License-Identifier: GPL-3.0-or-later
//! The order of fs-tree and extent-reference keys: object id, then record type, then the type's own key. Numbers in
//! a key compare as numbers, never as their little-endian bytes.

use std::cmp::Ordering;

use switch_apfs_format::clone_mapping::APFS_TYPE_CLONE_MAPPING;
use switch_apfs_format::fs::{APFS_TYPE_DIR_REC, APFS_TYPE_FILE_EXTENT, APFS_TYPE_XATTR, OBJ_ID_MASK, OBJ_TYPE_SHIFT};
use switch_apfs_format::fs_records::APFS_TYPE_SIBLING_LINK;

fn u32_at(k: &[u8], at: usize) -> u32 {
    k.get(at..at + 4)
        .and_then(|b| b.try_into().ok())
        .map_or(0, u32::from_le_bytes)
}

fn u64_at(k: &[u8], at: usize) -> u64 {
    k.get(at..at + 8)
        .and_then(|b| b.try_into().ok())
        .map_or(0, u64::from_le_bytes)
}

fn head(k: &[u8]) -> (u64, u8) {
    let v = u64_at(k, 0);
    (v & OBJ_ID_MASK, (v >> OBJ_TYPE_SHIFT) as u8)
}

pub fn fs_key_order(a: &[u8], b: &[u8]) -> Ordering {
    let (ha, hb) = (head(a), head(b));
    ha.cmp(&hb).then_with(|| match ha.1 {
        // A hashed name: the hash, then the name (APFS compares the bytes of names whose hashes collide).
        APFS_TYPE_DIR_REC => (u32_at(a, 8) >> 10)
            .cmp(&(u32_at(b, 8) >> 10))
            .then_with(|| a[12..].cmp(&b[12..])),
        // The name after its 16-bit length; macOS stores com.apple.ResourceFork before com.apple.decmpfs.
        APFS_TYPE_XATTR => a[10..].cmp(&b[10..]),
        APFS_TYPE_FILE_EXTENT | APFS_TYPE_SIBLING_LINK => u64_at(a, 8).cmp(&u64_at(b, 8)),
        APFS_TYPE_CLONE_MAPPING => (u64_at(a, 12), u64_at(a, 20)).cmp(&(u64_at(b, 12), u64_at(b, 20))),
        _ => a[8..].cmp(&b[8..]),
    })
}

pub fn sort(records: &mut [(Vec<u8>, Option<Vec<u8>>)]) {
    records.sort_by(|a, b| fs_key_order(&a.0, &b.0));
}
