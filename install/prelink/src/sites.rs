// SPDX-License-Identifier: GPL-3.0-or-later
//! A kind's declared instruction sites in Apple's kexts (`kits/SwitchABI/kinds/<kind>/sites/<bundle>.json`), read
//! and applied as `kits/SwitchABI/sites.py` does: every original word is checked before any is written.

use std::collections::HashSet;

use serde::Deserialize;

use crate::error::{Error, Result, json, refused};
use crate::pyint;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Site {
    pub bundle: String,
    /// Apple's address of the instruction.
    pub at: u64,
    pub original: u32,
    pub replacement: u32,
    pub why: String,
}

#[derive(Deserialize)]
struct Table {
    kind: String,
    bundle: String,
    sites: Vec<Entry>,
}

#[derive(Deserialize)]
struct Entry {
    at: String,
    from: String,
    to: String,
    why: String,
}

/// The sites of every table, each of which must be for `kind`. No address may be declared twice.
pub fn load_sites(tables: &[&[u8]], kind: &str) -> std::result::Result<Vec<Site>, Error> {
    let mut sites = Vec::new();
    for bytes in tables {
        let table: Table = json("a sites table", bytes)?;
        if table.kind != kind {
            return refused(format!("{}: sites for {}, not {kind}", table.bundle, table.kind));
        }
        for entry in table.sites {
            sites.push(Site {
                bundle: table.bundle.clone(),
                at: hex(&entry.at)?,
                original: hex(&entry.from)?,
                replacement: hex(&entry.to)?,
                why: entry.why,
            });
        }
    }
    let mut seen = HashSet::new();
    if let Some(twice) = sites.iter().find(|s| !seen.insert(s.at)) {
        return refused(format!("{:#x}: declared twice", twice.at));
    }
    Ok(sites)
}

fn hex<T: TryFrom<i128>>(text: &str) -> Result<T> {
    match pyint::parse(text, 16).map(T::try_from) {
        Some(Ok(value)) => Ok(value),
        _ => refused(format!("{text:?}: not a site address or instruction")),
    }
}

/// Writes each site's replacement into `code` once every site's original word is checked.
pub(crate) fn apply<T>(
    sites: &[Site],
    code: &mut T,
    read: impl Fn(&T, u64) -> Result<u32>,
    mut write: impl FnMut(&mut T, u64, u32) -> Result<()>,
) -> Result<()> {
    for site in sites {
        let found = read(code, site.at)?;
        if found != site.original {
            return refused(format!(
                "{} {:#x}: {found:08x} where the table has {:08x} ({})",
                site.bundle, site.at, site.original, site.why
            ));
        }
    }
    sites.iter().try_for_each(|site| write(code, site.at, site.replacement))
}
