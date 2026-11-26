// SPDX-License-Identifier: GPL-3.0-or-later
//! GPU stage format 2 (`tools/gpu/stage_format.py`) manifest reader and `switch_apfs_write::Tree` stager matching
//! `drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py`.

pub mod manifest;
mod plist_merge;
pub mod stage;
mod tar_source;

use std::io;
use std::path::PathBuf;

pub use manifest::{
    ADDONS_DIR, BUNDLE_DIR, FileContent, FileRecord, GpuStage, HEADER, ICON_CACHE_RECORD_PATH, KEXT_PREFIX,
    ManifestFile, ManifestForm, ParsedManifest, PrefFile, REQUIRED_PLUGIN_PATH, STATE_PATH, TREE_MANIFEST, check_path,
    json_to_plist,
};
pub use stage::{GpuStageReport, LARGE_FILE_THRESHOLD, stage_gpu, stage_gpu_from_path};

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
    #[error(transparent)]
    Image(#[from] switch_image::Error),
    #[error("gpu stage: {0}")]
    Manifest(String),
    #[error("gpu stage: {0} has no {TREE_MANIFEST} (format 2 manifest required)")]
    MissingTreeManifest(PathBuf),
    #[error("gpu stage: invalid path {0:?}")]
    InvalidPath(String),
    #[error("gpu stage: missing required plugin {REQUIRED_PLUGIN_PATH}")]
    MissingPlugin,
    #[error("gpu stage: duplicate file path {0:?}")]
    DuplicateFile(String),
    #[error("gpu stage: conflicting owners for {path}: {first} vs {second}")]
    ConflictingPrefOwner { path: String, first: u32, second: u32 },
    #[error("gpu stage: null JSON value is not valid in a property list")]
    NullPrefValue,
    #[error("gpu stage: missing source file {0}")]
    MissingSourceFile(String),
    #[error("gpu stage: {path}: size is {got} bytes, manifest expects {want}")]
    SizeMismatch { path: String, got: u64, want: u64 },
    #[error("gpu stage: {path}: SHA-256 is {got}, manifest expects {want}")]
    DigestMismatch { path: String, got: String, want: String },
    #[error("gpu stage: {0}: not a directory")]
    NotDirectory(String),
    #[error("gpu stage: {0}: not a regular file")]
    NotRegularFile(String),
    #[error("gpu stage: {0}: existing file is backed by source blocks, but no source Fs was provided")]
    MissingSourceFs(String),
    #[error("gpu stage: {path}: invalid property list: {detail}")]
    InvalidPlist { path: String, detail: String },
}

#[cfg(test)]
mod tests_manifest;
#[cfg(test)]
mod tests_source_plist;
#[cfg(test)]
mod tests_stage;
