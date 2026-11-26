// SPDX-License-Identifier: GPL-3.0-or-later
//! What the bindings export can't pin: the site tables and the translator. CI publishes, with each release, the
//! SHA-256 of every site table and of the prelinked kernel itself. The translator is compiled for the user's
//! platform by another compiler than CI's CLI, so only the prelinked kernel's hash pins it.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::error::{Error, Result, json, refused};

const FORMAT: u32 = 1;

#[derive(Deserialize, Serialize)]
pub(crate) struct Pins {
    format: u32,
    build: String,
    prelinked_sha256: String,
    /// Each site table's SHA-256, by the bundle it declares.
    sites: BTreeMap<String, String>,
}

#[derive(Deserialize)]
struct Table {
    bundle: String,
}

impl Pins {
    pub fn read(bytes: &[u8], build: &str) -> Result<Self> {
        let pins: Pins = json("the prelink pins", bytes)?;
        if pins.format != FORMAT {
            return refused(format!(
                "The prelink pins: format {}, this prelink reads format {FORMAT}",
                pins.format
            ));
        }
        if pins.build != build {
            return refused(format!(
                "The prelink pins are for {}, the bindings for {build}",
                pins.build
            ));
        }
        Ok(pins)
    }

    /// Refuses unless the tables are exactly the pinned ones.
    pub fn check_sites(&self, tables: &[&[u8]]) -> Result<()> {
        let given = digests(tables)?;
        for (bundle, digest) in &given {
            match self.sites.get(bundle) {
                Some(pinned) if pinned == digest => {}
                Some(pinned) => {
                    return refused(format!("{bundle}: sites table {digest}, the release pins {pinned}"));
                }
                None => return refused(format!("{bundle}: a sites table the release doesn't pin")),
            }
        }
        if let Some(missing) = self.sites.keys().find(|b| !given.contains_key(*b)) {
            return refused(format!("{missing}: the release pins a sites table not given"));
        }
        Ok(())
    }

    pub fn check_prelinked(&self, prelinked: &[u8]) -> Result<()> {
        let digest = hex::encode(Sha256::digest(prelinked));
        if digest != self.prelinked_sha256 {
            return refused(format!(
                "The prelinked kernel is {digest}, the release's is {}",
                self.prelinked_sha256
            ));
        }
        Ok(())
    }
}

/// The pins of a prelinked kernel and the site tables it was made with, as CI publishes them.
pub fn fileset_pins(build: &str, prelinked: &[u8], site_tables: &[&[u8]]) -> std::result::Result<Vec<u8>, Error> {
    let pins = Pins {
        format: FORMAT,
        build: build.to_owned(),
        prelinked_sha256: hex::encode(Sha256::digest(prelinked)),
        sites: digests(site_tables)?,
    };
    let mut out = serde_json::to_vec_pretty(&pins).expect("the pins serialize");
    out.push(b'\n');
    Ok(out)
}

fn digests(tables: &[&[u8]]) -> Result<BTreeMap<String, String>> {
    let mut digests = BTreeMap::new();
    for bytes in tables {
        let table: Table = json("a sites table", bytes)?;
        let digest = hex::encode(Sha256::digest(bytes));
        if digests.insert(table.bundle.clone(), digest).is_some() {
            return refused(format!("{}: two sites tables", table.bundle));
        }
    }
    Ok(digests)
}
