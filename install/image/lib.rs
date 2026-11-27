// SPDX-License-Identifier: GPL-3.0-or-later
//! Builds the Data and Preboot `switch_apfs_write::Tree` models and volume specs from Apple's System volume,
//! `/private/var` pretranslation deltas, and Preboot cryptex images.

pub mod data;
pub mod delta;
pub mod fill;
pub mod path;
pub mod preboot;
pub mod spec;

use std::io;

pub use data::{SeededData, seed_data};
pub use delta::{
    DeltaDir, DeltaEntry, DeltaFiles, Replacement, VarDeltaStats, apply_var_delta, is_staged_app, is_var_delta_entry,
    load_manifest, parse_manifest, read_plaintext_sha256, tree_entries, var_entries,
};
pub use fill::{BytesFill, FileFill, hash_file, stream_from_replacement};
pub use path::{add_file_at, descendant_dirs, ensure_dir, ensure_dir_under, split_parent, split_rel, write_file_at};
pub use preboot::{CryptexContent, CryptexImage, build_preboot, staging_name};
pub use spec::{
    DATA_INCOMPAT_FEATURES, DATA_ROCOMPAT_FEATURES, DEFAULT_DATA_NAME, DEFAULT_PREBOOT_NAME, DEFAULT_VOLUME_FEATURES,
    DataVolumeParams, PrebootVolumeParams, data_volume_spec, parse_uuid, preboot_volume_spec,
};

pub const MOBILE_UID: u32 = 501;
pub const MOBILE_GID: u32 = 501;
pub const SF_IMMUTABLE: u32 = 0x0002_0000;
pub const STAGING_DIR: &str = "Switch-Staging";
pub const VAR_PREFIX: &str = "/private/var";
pub const STAGED_APPS_PREFIX: &str = "/private/var/staged_system_apps/";

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Read(#[from] switch_apfs_read::ReadError),
    #[error(transparent)]
    Write(#[from] switch_apfs_write::WriteError),
    #[error(transparent)]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error("invalid path {0:?}")]
    InvalidPath(String),
    #[error("{0}: not a directory")]
    NotDirectory(String),
    #[error("{0}: not a regular file")]
    NotRegularFile(String),
    #[error("{0}: not in the System volume")]
    MissingInSource(String),
    #[error("{0}: not in the Data tree")]
    MissingInTree(String),
    #[error("delta manifest: {0}")]
    Manifest(String),
    #[error("invalid UUID {0:?}")]
    InvalidUuid(String),
    #[error("{path}: source SHA-256 is {got}, manifest expects {want}")]
    InputDigestMismatch { path: String, got: String, want: String },
    #[error("{path}: replacement SHA-256 is {got}, manifest expects {want}")]
    OutputDigestMismatch { path: String, got: String, want: String },
    #[error("{first} and {second} are hard links to one file with different delta entries")]
    ConflictingDelta { first: String, second: String },
    #[error("{name}: SHA-256 is {got}, expected {want}")]
    DigestMismatch { name: String, got: String, want: String },
    #[error("cryptex image: {0}")]
    InvalidCryptex(String),
}

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_delta;
