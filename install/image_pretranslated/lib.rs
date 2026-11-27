// SPDX-License-Identifier: GPL-3.0-or-later
//! Places iOS 27's pretranslated System binaries onto Data under `Switch/pretranslated/`, encodes the kernel's
//! `.index` (`SWOV` v2) and `.originals` (`SWOR` v1) tables, and formats `Switch/pretranslated.json`.

pub mod manifest;
pub mod overlay_index;
pub mod place;

use std::io;

pub use manifest::{Manifest, ManifestFile};
pub use overlay_index::{
    CDHASH_LEN, FILE_RECORD_SIZE, HEADER_SIZE, IndexEntry, IndexError, MAGIC, MAX_DIRECTORIES, MAX_FILES,
    MAX_INDEX_BYTES, MAX_ORIGINALS, MAX_PATH, NAME, ORIGINALS_HEADER_SIZE, ORIGINALS_MAGIC, ORIGINALS_NAME,
    ORIGINALS_VERSION, OriginalsPair, VERSION, check_path, decode, decode_originals, encode, encode_originals,
};
pub use place::{PlacedPretranslated, parse_cdhash_pairs, place_pretranslated};

pub const DATA_FOLDER: &str = "Switch";
pub const OVERLAY_DIR: &str = "Switch/pretranslated";
pub const OVERLAY_MANIFEST: &str = "Switch/pretranslated.json";
pub const OVERLAY_INDEX: &str = "Switch/pretranslated/.index";
pub const OVERLAY_ORIGINALS: &str = "Switch/pretranslated/.originals";
pub const STAGED_APPS: &str = switch_image::STAGED_APPS_PREFIX;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Image(#[from] switch_image::Error),
    #[error(transparent)]
    Read(#[from] switch_apfs_read::ReadError),
    #[error(transparent)]
    Write(#[from] switch_apfs_write::WriteError),
    #[error(transparent)]
    Index(#[from] IndexError),
    #[error("{0}: not in the System volume")]
    MissingInSource(String),
    #[error("{0}: not a regular file on the System volume")]
    NotRegularFile(String),
    #[error("{first} and {second} are one System file translated two ways")]
    ConflictingTranslation { first: String, second: String },
    #[error("{path}: invalid CDHash {value:?}")]
    InvalidCdhash { path: String, value: String },
    #[error("invalid cdhash-pairs line {0:?}")]
    InvalidCdhashPairLine(String),
    #[error("translated CDHash {translated} maps to two original CDHashes")]
    ConflictingCdhashPair { translated: String },
    #[error("staged system app translation {0} has no original CDHash in os.cdhash-pairs")]
    MissingOriginalCdhash(String),
}

#[cfg(test)]
mod tests_index;
#[cfg(test)]
mod tests_place;
