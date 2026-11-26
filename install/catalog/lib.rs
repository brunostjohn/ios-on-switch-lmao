// SPDX-License-Identifier: GPL-3.0-or-later
//! The release's catalog.json: one record per build, with everything the installer takes from `ipsw/pins.bzl` and the
//! release's assets for it. A new build is a new record.
//!
//! Two layers refuse what this installer can't use. [`Catalog::parse`] refuses a catalog of another format, or one
//! holding a field it doesn't know. [`Record::plan`] refuses one record whose steps, member roles, container formats
//! or asset roles it doesn't know, which leaves the other builds usable: a step or kind is data, a field is a format.

mod asset;
mod digest;
mod plan;
mod record;

#[cfg(test)]
mod tests;

pub use asset::{CATALOG_NAME, Checked, catalog_url};
pub use digest::{Digest, Sha256, Sha384};
pub use plan::{ASSET_ROLES, Build, Container, LaunchdPins, Prelink, Role, Steps, Unsupported};
pub use record::{Aea, Asset, Catalog, ContainerPins, Derived, Im4p, Ipsw, Member, Record, Volumes};

pub const FORMAT: u32 = 1;

/// The GitHub repository the releases are published in (`install/release.bzl`'s `REPO`).
pub const REPO: &str = env!("SWITCH_RELEASE_REPO");

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("the catalog is format {0}, and this installer reads format {FORMAT}: a newer installer reads it")]
    Format(u64),
    #[error("the catalog: {0}")]
    Json(String),
    #[error("the catalog lists {0} twice")]
    Duplicate(String),
    #[error("the catalog lists no {role} asset for {build}")]
    NoAsset { build: String, role: String },
    #[error("reading {0}: {1}")]
    Read(String, #[source] std::io::Error),
    #[error("{name} is {got_size} bytes with SHA-256 {got}; the catalog's is {size} bytes with {sha256}")]
    AssetMismatch {
        name: String,
        got_size: u64,
        got: String,
        size: u64,
        sha256: String,
    },
}
