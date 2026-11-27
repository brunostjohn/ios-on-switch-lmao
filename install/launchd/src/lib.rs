// SPDX-License-Identifier: GPL-3.0-or-later
//! iOS 17's launchd catalogue as `tools/package/launchd_catalog.py` makes it, byte for byte: Apple's
//! `/System/Library/xpc/launchd.plist` with the project's three LaunchDaemons added, its detached ad hoc signature,
//! the one-entry trust module for it, and SHA-256(plist) ‖ SHA-256(signature), which the ADT carries.

pub mod bplist;
mod jobs;

use sha2::{Digest, Sha256};
use switch_sign::{CDHASH_LEN, Detached, RUNTIME, sign_detached};
use switch_trustcache::{Entry, trust_module};

pub use jobs::{APPS_JOB_PATH, APPS_LABEL, JOB_PATH, Jobs, LABEL, SDWAKE_JOB_PATH, SDWAKE_LABEL};

use bplist::{Object, Plist};

pub const IDENTIFIER: &str = "com.brunostjohn.switch.launchd-catalog";
/// The catalogue's static trust entry: launch-constraint category 2, no flags.
const TRUST_CATEGORY: u8 = 2;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{name} job plist: {source}")]
    JobPlist { name: &'static str, source: plist::Error },
    #[error("native launchd catalogue or detached signature differs from the IPSW pins")]
    Pins,
    #[error("invalid {0}")]
    InvalidJob(&'static str),
    #[error("invalid native launchd catalogue")]
    InvalidCatalog,
    #[error("{0} is already in the native catalogue")]
    AlreadyThere(String),
    #[error(transparent)]
    Plist(#[from] bplist::Error),
    #[error(transparent)]
    Sign(#[from] switch_sign::Error),
    #[error(transparent)]
    Trust(#[from] switch_trustcache::Error),
}

/// The IPSW's pins for Apple's catalogue and its signature, as `pins.bzl` writes them: lowercase SHA-256 hex.
pub struct Pins<'a> {
    pub catalog_sha256: &'a str,
    pub signature_sha256: &'a str,
}

pub struct Catalog {
    pub plist: Vec<u8>,
    pub signature: Vec<u8>,
    pub trust: Vec<u8>,
    pub digest: [u8; 64],
    pub cdhash: [u8; CDHASH_LEN],
}

/// `read_pinned_catalog`: Apple's catalogue, once it and its signature match the pins.
pub fn check_pins<'a>(catalog: &'a [u8], signature: &[u8], pins: &Pins) -> Result<&'a [u8], Error> {
    if hex::encode(Sha256::digest(catalog)) != pins.catalog_sha256
        || hex::encode(Sha256::digest(signature)) != pins.signature_sha256
    {
        return Err(Error::Pins);
    }
    Ok(catalog)
}

/// `add_bootstrap_jobs`: the jobs go at the end of `LaunchDaemons`, which is changed in place, so a catalogue that
/// shares it elsewhere shows them there too.
pub fn add_bootstrap_jobs(catalog: &[u8], jobs: &Jobs) -> Result<Vec<u8>, Error> {
    let entries = jobs.checked()?;
    let mut plist = Plist::from_binary(catalog)?;
    let daemons = match plist.object(plist.root()) {
        Object::Dict(_) => plist.get(plist.root(), "LaunchDaemons").ok_or(Error::InvalidCatalog)?,
        _ => return Err(Error::InvalidCatalog),
    };
    let Object::Dict(existing) = plist.object(daemons) else {
        return Err(Error::InvalidCatalog);
    };
    if existing.is_empty()
        || existing
            .iter()
            .any(|(_, job)| !matches!(plist.object(*job), Object::Dict(_)))
    {
        return Err(Error::InvalidCatalog);
    }
    for (path, job) in entries {
        let label = job
            .as_dictionary()
            .and_then(|job| job.get("Label"))
            .and_then(|l| l.as_string());
        let label = label.expect("checked jobs have a string Label");
        if plist.get(daemons, path).is_some() || has_label(&plist, daemons, label) {
            return Err(Error::AlreadyThere(label.to_owned()));
        }
        let job = plist.import(job)?;
        plist.insert(daemons, path, job);
    }
    Ok(plist.to_binary()?)
}

fn has_label(plist: &Plist, daemons: bplist::Id, label: &str) -> bool {
    let Object::Dict(jobs) = plist.object(daemons) else {
        return false;
    };
    jobs.iter().any(
        |(_, job)| matches!(plist.get(*job, "Label").map(|l| plist.object(l)), Some(Object::String(l)) if l == label),
    )
}

/// `launchd_catalog.py`'s `main`, less the `codesign --verify` and `--display` checks, which stay in CI.
pub fn build(catalog: &[u8], signature: &[u8], pins: &Pins, jobs: &Jobs) -> Result<Catalog, Error> {
    let native = check_pins(catalog, signature, pins)?;
    let plist = add_bootstrap_jobs(native, jobs)?;
    let options = Detached {
        identifier: IDENTIFIER,
        flags: RUNTIME,
        page_size: 4096,
    };
    let signed = sign_detached(&plist, &options)?;
    let mut digest = [0; 64];
    digest[..32].copy_from_slice(&Sha256::digest(&plist));
    digest[32..].copy_from_slice(&Sha256::digest(&signed.bytes));
    let entry = Entry {
        cdhash: signed.cdhash,
        flags: 0,
        category: TRUST_CATEGORY,
    };
    Ok(Catalog {
        trust: trust_module([entry], None)?,
        plist,
        signature: signed.bytes,
        digest,
        cdhash: signed.cdhash,
    })
}
