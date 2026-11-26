// SPDX-License-Identifier: GPL-3.0-or-later
//! Version-2 trust cache modules (tools/trustcache/trustcache.py module_entries and cdhash_list).

use std::collections::BTreeSet;

use crate::Error;

pub(crate) const CDHASH_BYTES: usize = 20;
const VERSION: u32 = 2;
const HEADER_BYTES: usize = 24;
const ENTRY_BYTES: usize = 24;

fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]])
}

/// A module's CDHashes, checking that its entries fill it and ascend.
fn cdhashes(module: &[u8]) -> Result<Vec<&[u8]>, Error> {
    if module.len() < HEADER_BYTES {
        return Err(Error::TrustModule("shorter than its header"));
    }
    let count = u32_at(module, 20) as usize;
    if u32_at(module, 0) != VERSION || Some(module.len()) != count.checked_mul(ENTRY_BYTES).map(|n| n + HEADER_BYTES) {
        return Err(Error::TrustModule("not version 2 with the entries its header counts"));
    }
    let hashes: Vec<&[u8]> = module[HEADER_BYTES..]
        .chunks_exact(ENTRY_BYTES)
        .map(|entry| &entry[..CDHASH_BYTES])
        .collect();
    if hashes.windows(2).any(|pair| pair[0] >= pair[1]) {
        return Err(Error::TrustModule("entries not in ascending CDHash order"));
    }
    Ok(hashes)
}

/// The CDHashes of `modules`' entries back to back, ascending and each once.
pub(crate) fn cdhash_list(modules: &[&[u8]]) -> Result<Vec<u8>, Error> {
    let mut listed = BTreeSet::new();
    for module in modules {
        listed.extend(cdhashes(module)?);
    }
    Ok(listed.into_iter().flatten().copied().collect())
}
