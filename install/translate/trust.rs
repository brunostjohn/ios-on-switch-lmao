// SPDX-License-Identifier: GPL-3.0-or-later
//! Apple's trust caches, for each translated binary's flags and launch-constraint category, and the release's pins of
//! what a run must produce.

use std::collections::{BTreeMap, HashMap};

use sha2::{Digest, Sha256};

use crate::Error;
use crate::ffi::CDHASH;

/// CDHash → (flags, category), later modules overriding earlier ones.
pub type Categories = HashMap<[u8; CDHASH], (u8, u8)>;

/// The entries of Apple's trust cache modules (IM4P payloads), as translate_tree.py reads them.
pub fn categories(modules: &[Vec<u8>]) -> Result<Categories, Error> {
    let mut out = HashMap::new();
    for m in modules {
        let bad = || Error::Refused(format!("Apple's trust cache: {} bytes is no module", m.len()));
        let word = |at: usize| {
            m.get(at..at + 4)
                .map(|b| u32::from_le_bytes(b.try_into().expect("four bytes")))
        };
        let (version, count) = (word(0).ok_or_else(bad)?, word(20).ok_or_else(bad)?);
        let stride = if version == 2 { 24 } else { 22 };
        for i in 0..count as usize {
            let e = m.get(24 + stride * i..24 + stride * (i + 1)).ok_or_else(bad)?;
            let cdhash: [u8; CDHASH] = e[..CDHASH].try_into().expect("twenty bytes");
            out.insert(cdhash, (e[21], if version == 2 { e[22] } else { 0 }));
        }
    }
    Ok(out)
}

/// `{file name: sha256}` of a pretranslate run (`ipsw/pretranslated/<build>/`), as the release publishes it.
pub struct Pins(BTreeMap<String, String>);

impl Pins {
    pub fn parse(json: &[u8]) -> Result<Self, Error> {
        serde_json::from_slice(json)
            .map(Pins)
            .map_err(|e| Error::Refused(format!("the pretranslate pins: {e}")))
    }

    pub fn from_map(pins: BTreeMap<String, String>) -> Self {
        Pins(pins)
    }

    /// Refuses unless `bytes` is the file the pins name.
    pub fn check(&self, name: &str, bytes: &[u8]) -> Result<(), Error> {
        let got = hex::encode(Sha256::digest(bytes));
        match self.0.get(name) {
            Some(pinned) if pinned.eq_ignore_ascii_case(&got) => Ok(()),
            Some(pinned) => Err(Error::Refused(format!("{name} is {got}, the release pins {pinned}"))),
            None => Err(Error::Refused(format!("the release pins no {name}"))),
        }
    }
}
