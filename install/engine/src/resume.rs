// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::ops::Range;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::error::{EngineError, Result};

pub const RESUME_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BlockRange {
    pub start: u64,
    pub end: u64,
}

impl From<Range<u64>> for BlockRange {
    fn from(r: Range<u64>) -> Self {
        Self {
            start: r.start,
            end: r.end,
        }
    }
}

impl From<BlockRange> for Range<u64> {
    fn from(r: BlockRange) -> Self {
        r.start..r.end
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResumeState {
    pub version: u32,
    pub fingerprint: String,
    pub build: String,
    pub target_first_lba: u64,
    pub target_sectors: u64,
    pub stale_descriptor_areas: Vec<BlockRange>,
    pub phase_b_started: bool,
    #[serde(default)]
    pub completed_steps: BTreeMap<String, String>,
}

impl ResumeState {
    pub fn new(
        fingerprint: String,
        build: String,
        target_first_lba: u64,
        target_sectors: u64,
        stale: &[Range<u64>],
    ) -> Self {
        Self {
            version: RESUME_VERSION,
            fingerprint,
            build,
            target_first_lba,
            target_sectors,
            stale_descriptor_areas: stale.iter().cloned().map(BlockRange::from).collect(),
            phase_b_started: false,
            completed_steps: BTreeMap::new(),
        }
    }

    pub fn stale_ranges(&self) -> Vec<Range<u64>> {
        self.stale_descriptor_areas.iter().copied().map(Range::from).collect()
    }

    pub fn matches_partition(&self, fingerprint: &str, first_lba: u64, sectors: u64) -> bool {
        self.version == RESUME_VERSION
            && self.fingerprint == fingerprint
            && self.target_first_lba == first_lba
            && self.target_sectors == sectors
    }

    pub fn matches_build(&self, build: &str) -> bool {
        self.build == build
    }

    /// Invalidates build-scoped step markers when switching builds while preserving `stale_descriptor_areas`
    /// if phase B already zeroed block 0 on this partition.
    pub fn reconcile_build(&mut self, build: &str) {
        if self.build != build {
            self.build = build.to_owned();
            self.completed_steps.clear();
        }
    }

    pub fn mark_step(&mut self, step: &str, digest: impl Into<String>) {
        self.completed_steps.insert(step.to_owned(), digest.into());
    }

    pub fn step_matches(&self, step: &str, digest: &str) -> bool {
        self.completed_steps.get(step).is_some_and(|d| d == digest)
    }
}

pub fn partition_fingerprint(
    card_sectors: u64,
    entry_index: u32,
    first_lba: u64,
    sectors: u64,
    unique_guid: &[u8; 16],
) -> String {
    let mut hasher = Sha256::new();
    hasher.update(b"switch-engine-partition-v1\0");
    hasher.update(card_sectors.to_le_bytes());
    hasher.update(entry_index.to_le_bytes());
    hasher.update(first_lba.to_le_bytes());
    hasher.update(sectors.to_le_bytes());
    hasher.update(unique_guid);
    hex::encode(hasher.finalize())
}

pub struct ResumeStore {
    dir: PathBuf,
}

impl ResumeStore {
    pub fn new(work_dir: &Path) -> Self {
        Self {
            dir: work_dir.join("resume"),
        }
    }

    pub fn state_path(&self, fingerprint: &str) -> PathBuf {
        self.dir.join(format!("{fingerprint}.json"))
    }

    pub fn load(&self, fingerprint: &str, first_lba: u64, sectors: u64) -> Result<Option<ResumeState>> {
        let path = self.state_path(fingerprint);
        let bytes = match fs::read(&path) {
            Ok(b) => b,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(EngineError::io_path(&path, e)),
        };
        let state: ResumeState = match serde_json::from_slice(&bytes) {
            Ok(s) => s,
            Err(_) => return Ok(None),
        };
        if !state.matches_partition(fingerprint, first_lba, sectors) {
            return Ok(None);
        }
        Ok(Some(state))
    }

    pub fn save(&self, state: &ResumeState) -> Result<PathBuf> {
        fs::create_dir_all(&self.dir).map_err(|e| EngineError::io_path(&self.dir, e))?;
        let path = self.state_path(&state.fingerprint);
        let tmp = self.dir.join(format!("{}.json.tmp", state.fingerprint));
        let json = serde_json::to_vec_pretty(state)
            .map_err(|e| EngineError::ResumeState(format!("serialize resume state: {e}")))?;
        {
            let mut file = OpenOptions::new()
                .create(true)
                .truncate(true)
                .write(true)
                .open(&tmp)
                .map_err(|e| EngineError::io_path(&tmp, e))?;
            file.write_all(&json).map_err(|e| EngineError::io_path(&tmp, e))?;
            file.sync_data().map_err(|e| EngineError::io_path(&tmp, e))?;
        }
        fs::rename(&tmp, &path).map_err(|e| EngineError::io_path(&path, e))?;
        Ok(path)
    }

    pub fn clear(&self, fingerprint: &str) -> Result<()> {
        let path = self.state_path(fingerprint);
        match fs::remove_file(&path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(EngineError::io_path(&path, e)),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CachedFileStamp {
    pub size: u64,
    pub sha256: String,
}

impl CachedFileStamp {
    pub fn matches_file(&self, path: &Path, expected_size: u64, expected_sha256: &str) -> bool {
        if self.size != expected_size || !self.sha256.eq_ignore_ascii_case(expected_sha256) {
            return false;
        }
        fs::metadata(path).is_ok_and(|m| m.is_file() && m.len() == expected_size)
    }
}

pub fn load_stamp(stamp_path: &Path) -> Option<CachedFileStamp> {
    let bytes = fs::read(stamp_path).ok()?;
    serde_json::from_slice(&bytes).ok()
}

pub fn save_stamp(stamp_path: &Path, size: u64, sha256: &str) -> Result<()> {
    if let Some(parent) = stamp_path.parent() {
        fs::create_dir_all(parent).map_err(|e| EngineError::io_path(parent, e))?;
    }
    let stamp = CachedFileStamp {
        size,
        sha256: sha256.to_owned(),
    };
    let json = serde_json::to_vec(&stamp).map_err(|e| EngineError::ResumeState(format!("serialize stamp: {e}")))?;
    let tmp = stamp_path.with_extension("stamp.tmp");
    fs::write(&tmp, &json).map_err(|e| EngineError::io_path(&tmp, e))?;
    fs::rename(&tmp, stamp_path).map_err(|e| EngineError::io_path(stamp_path, e))
}
