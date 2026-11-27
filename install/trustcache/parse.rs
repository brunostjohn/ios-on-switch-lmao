// SPDX-License-Identifier: GPL-3.0-or-later
//! Reading modules: tools/trustcache/trustcache.py module_entries and cdhash_list.

use std::collections::BTreeSet;

use crate::{CDHASH_LEN, Error, VERSION};

pub const HEADER_LEN: usize = 24;
pub const ENTRY_LEN: usize = 24;

fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(data[at..at + 4].try_into().expect("four bytes"))
}

/// A module's 24-byte entries (CDHash, hash type, flags, category, reserved), checking that they fill it and ascend.
pub fn module_entries(module: &[u8]) -> Result<Vec<&[u8]>, Error> {
    if module.len() < HEADER_LEN {
        return Err(Error::Module(format!("{} bytes is no module", module.len())));
    }
    let (version, count) = (u32_at(module, 0), u32_at(module, 20));
    if version != VERSION || Some(module.len()) != (count as usize).checked_mul(ENTRY_LEN).map(|n| n + HEADER_LEN) {
        return Err(Error::Module(format!(
            "version {version} with {count} entries in {} bytes",
            module.len()
        )));
    }
    let entries: Vec<&[u8]> = module[HEADER_LEN..].chunks_exact(ENTRY_LEN).collect();
    if entries
        .windows(2)
        .any(|pair| pair[0][..CDHASH_LEN] >= pair[1][..CDHASH_LEN])
    {
        return Err(Error::Module("entries not in ascending CDHash order".into()));
    }
    Ok(entries)
}

/// The CDHashes of `modules`' entries back to back, ascending and each once.
pub fn cdhash_list(modules: &[&[u8]]) -> Result<Vec<u8>, Error> {
    let mut listed = BTreeSet::new();
    for module in modules {
        listed.extend(module_entries(module)?.into_iter().map(|entry| &entry[..CDHASH_LEN]));
    }
    Ok(listed.into_iter().flatten().copied().collect())
}
