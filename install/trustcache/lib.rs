// SPDX-License-Identifier: GPL-3.0-or-later
//! Version-2 trust cache modules, byte for byte as `tools/trustcache/trustcache.py` writes them. The kernel
//! binary-searches a module and refuses one that is unsorted or repeats a CDHash.

use std::collections::BTreeMap;

use sha2::{Digest, Sha256};

pub const CDHASH_LEN: usize = 20;
const VERSION: u32 = 2;
const HASH_TYPE_SHA256: u8 = 2;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("trust cache: {cdhash} given with flags/category {first:?} and {second:?}")]
    Conflict {
        cdhash: String,
        first: (u8, u8),
        second: (u8, u8),
    },
    #[error("trust cache: {0} entries do not fit a module")]
    TooMany(usize),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    pub cdhash: [u8; CDHASH_LEN],
    pub flags: u8,
    pub category: u8,
}

/// `trust_module(entries, uuid)`. The same CDHash may come more than once with the same flags and category.
/// Without `uuid` the module's is the first 16 bytes of its entries' SHA-256.
pub fn trust_module(entries: impl IntoIterator<Item = Entry>, uuid: Option<[u8; 16]>) -> Result<Vec<u8>, Error> {
    let mut unique = BTreeMap::new();
    for entry in entries {
        let given = (entry.flags, entry.category);
        let first = *unique.entry(entry.cdhash).or_insert(given);
        if first != given {
            return Err(Error::Conflict {
                cdhash: hex(&entry.cdhash),
                first,
                second: given,
            });
        }
    }
    let count = u32::try_from(unique.len()).map_err(|_| Error::TooMany(unique.len()))?;
    let mut body = Vec::with_capacity(unique.len() * (CDHASH_LEN + 4));
    for (cdhash, (flags, category)) in &unique {
        body.extend_from_slice(cdhash);
        body.extend_from_slice(&[HASH_TYPE_SHA256, *flags, *category, 0]);
    }
    let uuid = uuid.unwrap_or_else(|| Sha256::digest(&body)[..16].try_into().expect("SHA-256 is 32 bytes"));
    let mut module = Vec::with_capacity(24 + body.len());
    module.extend_from_slice(&VERSION.to_le_bytes());
    module.extend_from_slice(&uuid);
    module.extend_from_slice(&count.to_le_bytes());
    module.extend_from_slice(&body);
    Ok(module)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
