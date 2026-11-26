// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde::Deserialize;
use sha2::{Digest, Sha256};
use switch_apfs_read::{Entry as FsEntry, Fs, Stream, XattrData, stream};
use switch_apfs_write::{InodeRef, Kind, Tree};

use crate::fill::stream_from_replacement;
use crate::{Error, STAGED_APPS_PREFIX, VAR_PREFIX};

const VAR_SLASH: &str = "/private/var/";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeltaEntry {
    pub path: String,
    pub file: String,
    pub sha256_in: [u8; 32],
    pub sha256_out: [u8; 32],
    pub cdhash: Vec<String>,
}

#[derive(Clone, Debug)]
pub enum Replacement {
    Bytes(Arc<[u8]>),
    File(PathBuf),
}

pub trait DeltaFiles {
    fn load(&self, entry: &DeltaEntry) -> Result<Replacement, Error>;
}

pub struct DeltaDir(pub PathBuf);

impl DeltaFiles for DeltaDir {
    fn load(&self, entry: &DeltaEntry) -> Result<Replacement, Error> {
        let mut path = self.0.clone();
        for part in entry.file.split('/') {
            if part.is_empty() || part == "." || part == ".." || part.contains('\0') {
                return Err(Error::InvalidPath(entry.file.clone()));
            }
            path.push(part);
        }
        Ok(Replacement::File(path))
    }
}

impl<F> DeltaFiles for F
where
    F: Fn(&DeltaEntry) -> Result<Replacement, Error>,
{
    fn load(&self, entry: &DeltaEntry) -> Result<Replacement, Error> {
        self(entry)
    }
}

#[derive(Deserialize)]
struct RawManifest {
    files: Vec<RawEntry>,
}

#[derive(Deserialize)]
struct RawEntry {
    path: String,
    file: String,
    sha256_in: String,
    sha256_out: String,
    #[serde(default)]
    cdhash: Vec<String>,
}

pub fn load_manifest(path: &Path) -> Result<Vec<DeltaEntry>, Error> {
    let bytes = fs::read(path)?;
    parse_manifest(&bytes)
}

pub fn parse_manifest(json: &[u8]) -> Result<Vec<DeltaEntry>, Error> {
    let raw: RawManifest = serde_json::from_slice(json).map_err(|e| Error::Manifest(e.to_string()))?;
    raw.files
        .into_iter()
        .map(|e| {
            check_volume_path(&e.path)?;
            check_relative_file(&e.file)?;
            Ok(DeltaEntry {
                path: e.path,
                file: e.file,
                sha256_in: parse_sha256(&e.sha256_in, "sha256_in")?,
                sha256_out: parse_sha256(&e.sha256_out, "sha256_out")?,
                cdhash: e.cdhash,
            })
        })
        .collect()
}

fn check_volume_path(path: &str) -> Result<(), Error> {
    if !path.starts_with('/')
        || path.ends_with('/')
        || path.contains("//")
        || path.contains('\0')
        || path.split('/').any(|p| p == "." || p == "..")
    {
        return Err(Error::Manifest(format!("{path:?} is not an absolute volume path")));
    }
    Ok(())
}

fn check_relative_file(file: &str) -> Result<(), Error> {
    if file.is_empty()
        || file.starts_with('/')
        || file.ends_with('/')
        || file.contains("//")
        || file.contains('\0')
        || file.split('/').any(|p| p == "." || p == "..")
    {
        return Err(Error::Manifest(format!("{file:?} is not a relative delta file path")));
    }
    Ok(())
}

fn parse_sha256(text: &str, field: &str) -> Result<[u8; 32], Error> {
    let mut out = [0u8; 32];
    hex::decode_to_slice(text, &mut out).map_err(|_| Error::Manifest(format!("invalid {field}: {text:?}")))?;
    Ok(out)
}

pub fn is_staged_app(path: &str) -> bool {
    path.starts_with(STAGED_APPS_PREFIX)
}

pub fn is_var_delta_entry(path: &str) -> bool {
    path.starts_with(VAR_SLASH) && !is_staged_app(path)
}

pub fn var_entries(entries: &[DeltaEntry]) -> Vec<DeltaEntry> {
    entries
        .iter()
        .filter(|e| is_var_delta_entry(&e.path))
        .cloned()
        .collect()
}

/// Entries placed in the pretranslated tree: non-`/private/var/` files plus `/private/var/staged_system_apps/`.
pub fn tree_entries(entries: &[DeltaEntry]) -> Vec<DeltaEntry> {
    entries
        .iter()
        .filter(|e| !e.path.starts_with(VAR_SLASH) || is_staged_app(&e.path))
        .cloned()
        .collect()
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VarDeltaStats {
    pub matched: usize,
    pub replaced: usize,
    pub hard_links: usize,
}

struct Replaced {
    path: String,
    sha256_in: [u8; 32],
    sha256_out: [u8; 32],
}

/// Applies the `/private/var` portion of the `os` delta in place on `data`, skipping `/private/var/staged_system_apps/`.
pub fn apply_var_delta(
    data: &mut Tree,
    system: &Fs,
    entries: &[DeltaEntry],
    files: &dyn DeltaFiles,
) -> Result<VarDeltaStats, Error> {
    let mut stats = VarDeltaStats::default();
    let mut replaced: HashMap<InodeRef, Replaced> = HashMap::new();

    for entry in entries {
        if !is_var_delta_entry(&entry.path) {
            continue;
        }
        stats.matched += 1;

        let rel = entry
            .path
            .strip_prefix(VAR_PREFIX)
            .and_then(|s| s.strip_prefix('/'))
            .ok_or_else(|| Error::InvalidPath(entry.path.clone()))?;
        let data_inode = data
            .lookup(rel)
            .ok_or_else(|| Error::MissingInTree(entry.path.clone()))?;
        if data.inode(data_inode).kind != Kind::File {
            return Err(Error::NotRegularFile(entry.path.clone()));
        }

        if let Some(prev) = replaced.get(&data_inode) {
            if prev.sha256_out != entry.sha256_out || prev.sha256_in != entry.sha256_in {
                return Err(Error::ConflictingDelta {
                    first: prev.path.clone(),
                    second: entry.path.clone(),
                });
            }
            stats.hard_links += 1;
            continue;
        }

        let sys_oid = system
            .lookup(&entry.path)?
            .ok_or_else(|| Error::MissingInSource(entry.path.clone()))?;
        let (fs_entry, _) = system.entry(sys_oid, entry.path.clone(), None)?;
        if !fs_entry.inode.is_regular() {
            return Err(Error::NotRegularFile(entry.path.clone()));
        }
        let got_in = read_plaintext_sha256(system, &fs_entry)?;
        if got_in != entry.sha256_in {
            return Err(Error::InputDigestMismatch {
                path: entry.path.clone(),
                got: hex::encode(got_in),
                want: hex::encode(entry.sha256_in),
            });
        }

        let replacement = files.load(entry)?;
        let stream = stream_from_replacement(data, &entry.path, replacement, entry.sha256_out)?;
        data.replace_content(data_inode, stream)?;

        replaced.insert(
            data_inode,
            Replaced {
                path: entry.path.clone(),
                sha256_in: entry.sha256_in,
                sha256_out: entry.sha256_out,
            },
        );
        stats.replaced += 1;
    }

    Ok(stats)
}

pub fn read_plaintext_sha256(system: &Fs, entry: &FsEntry) -> Result<[u8; 32], Error> {
    if !entry.inode.is_regular() {
        return Err(Error::NotRegularFile(entry.path.clone()));
    }
    if entry.inode.is_compressed() {
        let decmpfs = read_xattr_bytes(system, entry, switch_decmpfs::XATTR)?.ok_or_else(|| {
            Error::Read(switch_apfs_read::ReadError::invalid(format!(
                "{}: missing decmpfs",
                entry.path
            )))
        })?;
        let fork = read_xattr_bytes(system, entry, switch_decmpfs::RESOURCE_FORK)?;
        let plain = switch_decmpfs::decompress(&decmpfs, fork.as_deref())?;
        return Ok(Sha256::digest(&plain).into());
    }
    let Some(s) = &entry.data else {
        return Ok(Sha256::digest([]).into());
    };
    let mut hasher = Sha256::new();
    stream::read(system.source(), s, &mut |chunk| hasher.update(chunk))?;
    Ok(hasher.finalize().into())
}

fn read_xattr_bytes(system: &Fs, entry: &FsEntry, name: &str) -> Result<Option<Vec<u8>>, Error> {
    let Some((xattr, xstream)) = entry.xattr(name) else {
        return Ok(None);
    };
    match (&xattr.data, xstream) {
        (XattrData::Embedded(bytes), _) => Ok(Some(bytes.clone())),
        (XattrData::Stream { .. }, Some(s)) => Ok(Some(read_stream_vec(system, s)?)),
        (XattrData::Stream { id, .. }, None) => Err(Error::Read(switch_apfs_read::ReadError::invalid(format!(
            "{}: {name} stream {id} missing extents",
            entry.path
        )))),
    }
}

fn read_stream_vec(system: &Fs, s: &Stream) -> Result<Vec<u8>, Error> {
    let size = usize::try_from(s.size).map_err(|_| {
        Error::Read(switch_apfs_read::ReadError::invalid(format!(
            "stream {}: {} bytes don't fit in memory",
            s.id, s.size
        )))
    })?;
    let mut out = Vec::with_capacity(size);
    stream::read(system.source(), s, &mut |chunk| out.extend_from_slice(chunk))?;
    Ok(out)
}
