// SPDX-License-Identifier: GPL-3.0-or-later
//! Rootless Procursus bootstrap staging onto `switch_apfs_write::Tree`, porting `tools/install/bootstrap.py` and
//! `tools/install/bootstrap_migration.py`.

pub mod archive;
pub mod dpkg;
pub mod json_py;
pub mod manifest;
pub mod stage;

pub use archive::{MetadataEntry, TarEntryKind, TarMember, build_metadata, normalize_archive_name, unpacked_bytes};
pub use dpkg::{ControlRecord, parse_control, rewrite_status};
pub use json_py::format_python_json;
pub use manifest::{
    ARCHIVE, BootstrapManifest, BootstrapPins, DIRECTORY, HELD, HELPER, MANIFEST, METADATA, MIGRATION_PENDING, ROOT,
    is_sha256_hex, parse_manifest,
};
pub use stage::{BootstrapInput, BootstrapReport, stage_bootstrap, stage_bootstrap_from_dir, stage_bootstrap_reader};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Write(#[from] switch_apfs_write::WriteError),
    #[error("bootstrap: {0}")]
    Manifest(String),
    #[error("bootstrap: {0}")]
    Digest(String),
    #[error("bootstrap: {0}")]
    Archive(String),
    #[error("bootstrap: {0}")]
    Dpkg(String),
    #[error("bootstrap: {0}")]
    Tree(String),
}

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_archive;
#[cfg(test)]
mod tests_dpkg;
