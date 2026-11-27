// SPDX-License-Identifier: GPL-3.0-or-later
//! The kernel window's trust cache bundle (tools/package/trust.py): every module merged into the IPSW's static one,
//! which keeps its UUID, as one module.

use switch_trustcache::{CDHASH_LEN, Entry, bundle, merged_module, trust_module};

use crate::Error;

/// What boot/defs.bzl's BootTrust step merges. The release ships the overlay's module without the CDHash of the
/// installd made from the user's IPSW, which `installd` gives back; merging sorts, dedupes and keeps only the static
/// module's UUID, so the bundle is the build's.
pub struct Bundle<'a> {
    /// The IPSW's StaticTrustCache payload.
    pub static_module: &'a [u8],
    /// The cryptexes', the overlay's (without installd), iOS 17's launchd catalogue's, the GPU bundle's and iOS 27's
    /// pretranslated trees' modules. An empty one adds nothing.
    pub modules: &'a [&'a [u8]],
    pub installd: [u8; CDHASH_LEN],
}

/// The bundle /chosen/memory-map/TrustCache points at.
pub fn trust_bundle(given: &Bundle) -> Result<Vec<u8>, Error> {
    // Flags and launch-constraint category 0, as overlay.py trust gives every overlay file: overlay code lives on Data,
    // where the system categories' volume constraints would refuse it.
    let installd = trust_module(
        [Entry {
            cdhash: given.installd,
            flags: 0,
            category: 0,
        }],
        None,
    )?;
    let mut modules = vec![given.static_module];
    modules.extend(given.modules.iter().copied().filter(|module| !module.is_empty()));
    modules.push(&installd);
    Ok(bundle(&merged_module(&modules)?)?)
}
