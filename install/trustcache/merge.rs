// SPDX-License-Identifier: GPL-3.0-or-later
//! Merging modules and the kernel window's bundle: trustcache.py merged_module, tools/package/trust.py
//! trust_cache_bundle and native_xnu_layout.py validate_trustcache.

use std::collections::BTreeMap;

use crate::parse::{ENTRY_LEN, HEADER_LEN, module_entries};
use crate::{CDHASH_LEN, Error, VERSION, hex};

/// One module holding every entry of `modules` under the first one's UUID. Entries keep their hash type, flags and
/// category; one in several modules stands once, and the same CDHash with two different entries is an error.
pub fn merged_module(modules: &[&[u8]]) -> Result<Vec<u8>, Error> {
    let first = modules.first().ok_or(Error::NoModules)?;
    let mut unique: BTreeMap<&[u8], &[u8]> = BTreeMap::new();
    for module in modules {
        for entry in module_entries(module)? {
            let cdhash = &entry[..CDHASH_LEN];
            if *unique.entry(cdhash).or_insert(entry) != entry {
                return Err(Error::TwoEntries(hex(cdhash)));
            }
        }
    }
    let count = u32::try_from(unique.len()).map_err(|_| Error::TooMany(unique.len()))?;
    let mut merged = Vec::with_capacity(HEADER_LEN + unique.len() * ENTRY_LEN);
    merged.extend_from_slice(&VERSION.to_le_bytes());
    merged.extend_from_slice(&first[4..20]);
    merged.extend_from_slice(&count.to_le_bytes());
    unique.values().for_each(|entry| merged.extend_from_slice(entry));
    Ok(merged)
}

/// /chosen/memory-map/TrustCache's bundle of one module: u32 count 1, u32 offset 8, the module.
pub fn bundle(module: &[u8]) -> Result<Vec<u8>, Error> {
    module_entries(module)?;
    let mut bundle = Vec::with_capacity(8 + module.len());
    bundle.extend_from_slice(&1u32.to_le_bytes());
    bundle.extend_from_slice(&8u32.to_le_bytes());
    bundle.extend_from_slice(module);
    Ok(bundle)
}

fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(data[at..at + 4].try_into().expect("four bytes"))
}

/// What the layout accepts as a bundle: 1 to 128 version-2 modules, each in its range and ascending, with a SHA-1 to
/// SHA-384 hash type on every entry.
pub fn check_bundle(data: &[u8]) -> Result<(), Error> {
    if data.len() < 8 {
        return Err(Error::Bundle("truncated trustcache table"));
    }
    let count = u32_at(data, 0) as usize;
    if !(1..=128).contains(&count) || count > (data.len() - 4) / 4 {
        return Err(Error::Bundle("invalid trustcache count"));
    }
    let starts: Vec<usize> = (0..count).map(|i| u32_at(data, 4 + 4 * i) as usize).collect();
    let ends = starts.iter().skip(1).copied().chain([data.len()]);
    for (start, end) in starts.iter().copied().zip(ends) {
        if start < 4 + count * 4 || start % 4 != 0 || end > data.len() || end < start + HEADER_LEN {
            return Err(Error::Bundle("invalid trustcache range"));
        }
        if module_entries(&data[start..end])?
            .iter()
            .any(|entry| !(1..=4).contains(&entry[CDHASH_LEN]))
        {
            return Err(Error::Bundle("invalid hash type"));
        }
    }
    Ok(())
}
