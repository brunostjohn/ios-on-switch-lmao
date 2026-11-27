// SPDX-License-Identifier: GPL-3.0-or-later
//! Rebuilds and seals a single-volume APFS cryptex image with a replacement delta in one pass, fitted to its final
//! container block count.

mod blocks;
pub mod delta;
pub mod file_info;
pub mod layout;
mod place;
pub mod records;
pub mod source;
pub mod write;

use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use switch_apfs_alloc::GeometryError;
use switch_apfs_alloc::space::SpaceError;
use switch_apfs_btree::BuildError;
use switch_apfs_format::io::{BlockSource, BlockTarget, FileTarget};
use switch_apfs_read::ReadError;
use switch_apfs_write::Fill;

pub use delta::load_manifest;
pub use file_info::{ChunkHash, FILE_INFO_CHUNK_SIZE, HashedReplacement};
pub use layout::{SealPlan, SealStats};

#[derive(Debug, thiserror::Error)]
pub enum SealError {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Read(#[from] ReadError),
    #[error(transparent)]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error(transparent)]
    Geometry(#[from] GeometryError),
    #[error(transparent)]
    Space(#[from] SpaceError),
    #[error(transparent)]
    Build(#[from] BuildError),
    #[error("invalid source cryptex: {0}")]
    InvalidSource(String),
    #[error("invalid path: {0}")]
    InvalidPath(String),
    #[error("{0} is not in the source cryptex")]
    MissingPath(String),
    #[error("{0} is not a regular file")]
    NotRegularFile(String),
    #[error("duplicate replacement path: {0}")]
    DuplicatePath(String),
    #[error("{first} and {second} are hard links to one file replaced two ways")]
    ConflictingHardLink { first: String, second: String },
    #[error("{path} has extended attributes the replacement would not carry: {}", xattrs.join(", "))]
    ReplacedHasXattrs { path: String, xattrs: Vec<String> },
    #[error("{path}: source sha256 {actual} does not match expected {expected}")]
    Sha256InMismatch {
        path: String,
        expected: String,
        actual: String,
    },
    #[error("{path}: replacement sha256 {actual} does not match expected {expected}")]
    Sha256OutMismatch {
        path: String,
        expected: String,
        actual: String,
    },
    #[error("invalid options: {0}")]
    InvalidSpec(String),
}

#[derive(Clone, Debug)]
pub enum ReplacementData {
    Bytes(Arc<[u8]>),
    File(PathBuf),
}

#[derive(Clone, Debug)]
pub struct Replacement {
    pub path: String,
    pub data: ReplacementData,
    pub sha256_in: Option<[u8; 32]>,
    pub sha256_out: Option<[u8; 32]>,
}

#[derive(Clone, Debug)]
pub struct SealOptions {
    pub volume_uuid: [u8; 16],
    pub volume_name: Option<String>,
    pub time_ns: u64,
    pub formatted_by: String,
}

impl SealOptions {
    pub fn new(volume_uuid: [u8; 16], time_ns: u64) -> Self {
        Self {
            volume_uuid,
            volume_name: None,
            time_ns,
            formatted_by: "switch_apfs_seal".to_owned(),
        }
    }
}

pub fn plan(src: &dyn BlockSource, replacements: &[Replacement], options: &SealOptions) -> Result<SealPlan, SealError> {
    let cryptex = source::SourceCryptex::read(src)?;
    let resolved = delta::resolve(src, &cryptex, replacements)?;
    let built_records = records::build(&cryptex, &resolved, options.time_ns)?;
    layout::build(&cryptex, built_records, resolved.len() as u64, options)
}

pub fn seal(
    src: &dyn BlockSource,
    replacements: &[Replacement],
    options: &SealOptions,
    target: &mut dyn BlockTarget,
) -> Result<SealStats, SealError> {
    let planned = plan(src, replacements, options)?;
    planned.write(src, target)?;
    Ok(planned.stats().clone())
}

pub fn seal_to_file(
    src: &dyn BlockSource,
    replacements: &[Replacement],
    options: &SealOptions,
    out: &Path,
) -> Result<SealStats, SealError> {
    let planned = plan(src, replacements, options)?;
    let mut target = FileTarget::create(out, planned.exact_size())?;
    if let Err(err) = planned.write(src, &mut target) {
        drop(target);
        let _ = std::fs::remove_file(out);
        return Err(err);
    }
    Ok(planned.stats().clone())
}

/// Streams a planned sealed cryptex straight into a Preboot extent via `switch_apfs_write::Fill`.
pub struct SealFill {
    src: Arc<dyn BlockSource + Send + Sync>,
    plan: SealPlan,
}

impl SealFill {
    pub fn new(src: Arc<dyn BlockSource + Send + Sync>, plan: SealPlan) -> Self {
        Self { src, plan }
    }

    pub fn exact_blocks(&self) -> u64 {
        self.plan.exact_blocks()
    }

    pub fn exact_size(&self) -> u64 {
        self.plan.exact_size()
    }

    pub fn plan(&self) -> &SealPlan {
        &self.plan
    }
}

impl Fill for SealFill {
    fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
        write::write_sequential(self.src.as_ref(), &self.plan, window).map_err(io::Error::other)
    }
}

pub fn seal_fill(
    src: Arc<dyn BlockSource + Send + Sync>,
    replacements: &[Replacement],
    options: &SealOptions,
) -> Result<SealFill, SealError> {
    let planned = plan(src.as_ref(), replacements, options)?;
    Ok(SealFill::new(src, planned))
}

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_fixture;
#[cfg(test)]
mod tests_inputs;
#[cfg(test)]
mod tests_regressions;
