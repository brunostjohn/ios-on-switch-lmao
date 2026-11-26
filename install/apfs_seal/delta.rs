// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Component, Path, PathBuf};

use serde::Deserialize;
use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME};
use switch_apfs_format::io::BlockSource;
use unicode_normalization::UnicodeNormalization;

use crate::file_info::{self, HashedReplacement};
use crate::source::{SourceCryptex, split_volume_path};
use crate::{Replacement, ReplacementData, SealError};

#[derive(Clone, Debug)]
pub struct ResolvedReplacement {
    pub path: String,
    pub data: ReplacementData,
    pub hashed: HashedReplacement,
}

pub fn resolve(
    src: &dyn BlockSource,
    cryptex: &SourceCryptex,
    replacements: &[Replacement],
) -> Result<BTreeMap<u64, ResolvedReplacement>, SealError> {
    let mut resolved: BTreeMap<u64, ResolvedReplacement> = BTreeMap::new();
    let mut seen_paths: HashSet<String> = HashSet::new();
    let mut cached_in: HashMap<u64, [u8; 32]> = HashMap::new();

    for entry in replacements {
        split_volume_path(&entry.path)?;
        let norm_path: String = entry.path.nfd().collect();
        if !seen_paths.insert(norm_path) {
            return Err(SealError::DuplicatePath(entry.path.clone()));
        }
        let oid = cryptex
            .lookup(&entry.path)?
            .ok_or_else(|| SealError::MissingPath(entry.path.clone()))?;
        let inode = cryptex
            .inodes
            .get(&oid)
            .ok_or_else(|| SealError::InvalidSource(format!("{}: missing inode {oid}", entry.path)))?;
        if !inode.is_regular() {
            return Err(SealError::NotRegularFile(entry.path.clone()));
        }

        // Match listxattr without XATTR_SHOWCOMPRESSION: compression xattrs are hidden on compressed files.
        let disallowed: Vec<String> = cryptex
            .xattrs
            .get(&oid)
            .into_iter()
            .flatten()
            .filter(|x| {
                !(inode.is_compressed() && (x.name == DECMPFS_XATTR_NAME || x.name == RESOURCE_FORK_XATTR_NAME))
            })
            .map(|x| x.name.clone())
            .collect();
        if !disallowed.is_empty() {
            return Err(SealError::ReplacedHasXattrs {
                path: entry.path.clone(),
                xattrs: disallowed,
            });
        }

        if let Some(existing) = resolved.get(&oid)
            && let Some(want_out) = entry.sha256_out
            && existing.hashed.sha256 != want_out
        {
            return Err(SealError::ConflictingHardLink {
                first: existing.path.clone(),
                second: entry.path.clone(),
            });
        }

        let hashed = file_info::hash_data(&entry.data)?;
        if let Some(want_out) = entry.sha256_out
            && hashed.sha256 != want_out
        {
            return Err(SealError::Sha256OutMismatch {
                path: entry.path.clone(),
                expected: hex::encode(want_out),
                actual: hex::encode(hashed.sha256),
            });
        }

        if let Some(want_in) = entry.sha256_in {
            let actual_in = match cached_in.get(&oid) {
                Some(&digest) => digest,
                None => {
                    let digest = cryptex.plaintext_sha256(src, oid, &entry.path)?;
                    cached_in.insert(oid, digest);
                    digest
                }
            };
            if actual_in != want_in {
                return Err(SealError::Sha256InMismatch {
                    path: entry.path.clone(),
                    expected: hex::encode(want_in),
                    actual: hex::encode(actual_in),
                });
            }
        }

        if let Some(existing) = resolved.get(&oid) {
            if existing.hashed.sha256 != hashed.sha256 {
                return Err(SealError::ConflictingHardLink {
                    first: existing.path.clone(),
                    second: entry.path.clone(),
                });
            }
            continue;
        }

        resolved.insert(
            oid,
            ResolvedReplacement {
                path: entry.path.clone(),
                data: entry.data.clone(),
                hashed,
            },
        );
    }

    Ok(resolved)
}

#[derive(Deserialize)]
struct ManifestJson {
    files: Vec<ManifestEntry>,
}

#[derive(Deserialize)]
struct ManifestEntry {
    path: String,
    file: String,
    sha256_in: Option<String>,
    sha256_out: Option<String>,
}

pub fn load_manifest(manifest_path: &Path) -> Result<Vec<Replacement>, SealError> {
    let text = fs::read_to_string(manifest_path)?;
    let parsed: ManifestJson =
        serde_json::from_str(&text).map_err(|e| SealError::InvalidSpec(format!("{}: {e}", manifest_path.display())))?;
    let base = manifest_path.parent().unwrap_or_else(|| Path::new("."));
    let mut out = Vec::with_capacity(parsed.files.len());
    for e in parsed.files {
        split_volume_path(&e.path)?;
        let rel = validate_manifest_file(&e.file)?;
        let sha256_in = e.sha256_in.as_deref().map(parse_sha256).transpose()?;
        let sha256_out = e.sha256_out.as_deref().map(parse_sha256).transpose()?;
        out.push(Replacement {
            path: e.path,
            data: ReplacementData::File(base.join(rel)),
            sha256_in,
            sha256_out,
        });
    }
    Ok(out)
}

fn validate_manifest_file(rel: &str) -> Result<PathBuf, SealError> {
    if rel.is_empty() || rel.contains('\0') {
        return Err(SealError::InvalidSpec(format!("invalid manifest file path: {rel:?}")));
    }
    let mut clean = PathBuf::new();
    let mut has_normal = false;
    for comp in Path::new(rel).components() {
        match comp {
            Component::Normal(part) => {
                clean.push(part);
                has_normal = true;
            }
            Component::CurDir => {}
            Component::Prefix(_) | Component::RootDir | Component::ParentDir => {
                return Err(SealError::InvalidSpec(format!(
                    "manifest file path escapes manifest directory: {rel:?}"
                )));
            }
        }
    }
    if !has_normal {
        return Err(SealError::InvalidSpec(format!("invalid manifest file path: {rel:?}")));
    }
    Ok(clean)
}

fn is_sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// Parses `VOLUME_PATH=HOST_FILE[:SHA256_IN[:SHA256_OUT]]` from the right so Windows drive letters and colons
/// inside host paths are preserved.
pub fn parse_replace(spec: &str) -> Result<Replacement, SealError> {
    let (path, rest) = spec
        .split_once('=')
        .ok_or_else(|| SealError::InvalidSpec(format!("expected PATH=FILE[:SHA256_IN[:SHA256_OUT]], got {spec}")))?;
    let (file, sha256_in, sha256_out) = if let Some((before_last, last)) = rest.rsplit_once(':')
        && is_sha256_hex(last)
    {
        let h_last = parse_sha256(last)?;
        if let Some((before_prev, prev)) = before_last.rsplit_once(':')
            && (prev.is_empty() || is_sha256_hex(prev))
        {
            let h_in = (!prev.is_empty()).then(|| parse_sha256(prev)).transpose()?;
            (before_prev, h_in, Some(h_last))
        } else {
            (before_last, Some(h_last), None)
        }
    } else if let Some(before_empty) = rest.strip_suffix(':')
        && let Some((before_prev, prev)) = before_empty.rsplit_once(':')
        && is_sha256_hex(prev)
    {
        (before_prev, Some(parse_sha256(prev)?), None)
    } else {
        (rest, None, None)
    };
    if file.is_empty() {
        return Err(SealError::InvalidSpec(format!("missing host file in {spec}")));
    }
    Ok(Replacement {
        path: path.to_owned(),
        data: ReplacementData::File(PathBuf::from(file)),
        sha256_in,
        sha256_out,
    })
}

pub fn parse_sha256(s: &str) -> Result<[u8; 32], SealError> {
    let bytes = hex::decode(s).map_err(|_| SealError::InvalidSpec(format!("invalid sha256 hex: {s}")))?;
    bytes
        .try_into()
        .map_err(|_| SealError::InvalidSpec(format!("sha256 must be 64 hex chars: {s}")))
}
