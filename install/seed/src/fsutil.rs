// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::error::{Result, SeedError};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub(crate) struct FileDigest {
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct OutputStamp {
    pub files: BTreeMap<String, FileDigest>,
}

pub(crate) struct CacheLock {
    _file: File,
}

impl CacheLock {
    pub(crate) fn acquire(work_dir: &Path) -> Result<Self> {
        fs::create_dir_all(work_dir).map_err(|e| SeedError::io_path(work_dir, e))?;
        let lock_path = work_dir.join(".seed.lock");
        let file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(false)
            .open(&lock_path)
            .map_err(|e| SeedError::io_path(&lock_path, e))?;
        file.lock().map_err(|e| SeedError::io_path(&lock_path, e))?;
        Ok(Self { _file: file })
    }
}

pub(crate) fn check_cancel(cancel: &AtomicBool) -> Result<()> {
    if cancel.load(Ordering::Relaxed) {
        Err(SeedError::Cancelled)
    } else {
        Ok(())
    }
}

pub(crate) fn unique_temp_sibling(target: &Path, label: &str) -> PathBuf {
    static SERIAL: AtomicU64 = AtomicU64::new(0);
    let seq = SERIAL.fetch_add(1, Ordering::Relaxed);
    let name = target.file_name().and_then(|s| s.to_str()).unwrap_or(label);
    target.with_file_name(format!(".{name}.{}-{seq}.tmp", std::process::id()))
}

pub(crate) fn replace_file_atomic(tmp: &Path, dst: &Path) -> Result<()> {
    #[cfg(windows)]
    {
        if dst.exists() {
            let _ = fs::remove_file(dst);
        }
    }
    fs::rename(tmp, dst).map_err(|e| SeedError::io_path(dst, e))
}

pub(crate) fn replace_dir_atomic(tmp: &Path, dst: &Path) -> Result<()> {
    if dst.exists() || fs::symlink_metadata(dst).is_ok() {
        let meta = fs::symlink_metadata(dst).map_err(|e| SeedError::io_path(dst, e))?;
        if meta.file_type().is_symlink() || meta.is_file() {
            fs::remove_file(dst).map_err(|e| SeedError::io_path(dst, e))?;
        } else {
            fs::remove_dir_all(dst).map_err(|e| SeedError::io_path(dst, e))?;
        }
    }
    fs::rename(tmp, dst).map_err(|e| SeedError::io_path(dst, e))
}

pub(crate) fn verify_inventory(root: &Path, files: &BTreeMap<String, FileDigest>) -> bool {
    for (rel, want) in files {
        let path = root.join(rel);
        let Ok(meta) = fs::symlink_metadata(&path) else {
            return false;
        };
        if meta.file_type().is_symlink() || !meta.is_file() || meta.len() != want.size {
            return false;
        }
        let Ok((sz, sha)) = sha256_file(&path) else {
            return false;
        };
        if sz != want.size || !hex::encode(sha).eq_ignore_ascii_case(&want.sha256) {
            return false;
        }
    }
    true
}

pub(crate) fn collect_sorted_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(dir).map_err(|e| SeedError::io_path(dir, e))? {
        let path = entry?.path();
        let meta = fs::symlink_metadata(&path).map_err(|e| SeedError::io_path(&path, e))?;
        if meta.file_type().is_symlink() {
            continue;
        }
        if meta.is_dir() {
            collect_sorted_files(&path, out)?;
        } else if meta.is_file() {
            out.push(path);
        }
    }
    out.sort();
    Ok(())
}

pub(crate) fn collect_inventory(root: &Path) -> Result<BTreeMap<String, FileDigest>> {
    let mut all_files = Vec::new();
    collect_sorted_files(root, &mut all_files)?;
    let mut files = BTreeMap::new();
    for p in all_files {
        let rel = p
            .strip_prefix(root)
            .map_err(|e| SeedError::Asset(e.to_string()))?
            .to_string_lossy()
            .replace('\\', "/");
        let (size, sha) = sha256_file(&p)?;
        files.insert(
            rel,
            FileDigest {
                size,
                sha256: hex::encode(sha),
            },
        );
    }
    Ok(files)
}

pub(crate) fn sha256_file(path: &Path) -> Result<(u64, [u8; 32])> {
    let file = File::open(path).map_err(|e| SeedError::io_path(path, e))?;
    let mut reader = BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| SeedError::io_path(path, e))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        total += n as u64;
    }
    Ok((total, hasher.finalize().into()))
}
