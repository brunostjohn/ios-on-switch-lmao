// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{BufReader, Cursor, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use switch_seed_crawl::{rule_for_build, validate_portable_rel_path};

use crate::error::{Result, SeedError};

pub const MANIFEST_FORMAT: u32 = 1;
pub const EXPECTED_SPLIT_BUCKETS: usize = 17;
const MAX_MANIFEST_BYTES: u64 = 4 << 20;
const MAX_ENTRY_BYTES: u64 = 256 << 20;
const MAX_TOTAL_BYTES: u64 = 1024 << 20;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SeedAssetFile {
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SeedAppLibrary {
    pub guest_path: String,
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SeedBucketCounts {
    pub lines: Vec<usize>,
    pub split: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SeedAssetManifest {
    pub format: u32,
    pub build: String,
    pub kind: String,
    pub stack_key: String,
    pub quartzcore_metallib_sha256: String,
    pub bucket_counts: SeedBucketCounts,
    #[serde(default)]
    pub embedded: Vec<SeedAssetFile>,
    #[serde(default)]
    pub app_libraries: Vec<SeedAppLibrary>,
    pub files: Vec<SeedAssetFile>,
}

#[derive(Clone, Debug)]
pub struct UnpackedSeedAsset {
    pub manifest: SeedAssetManifest,
    pub root: PathBuf,
    pub own_metallib: PathBuf,
    pub context_log: PathBuf,
    pub draw_context: PathBuf,
    pub corpus_log: PathBuf,
    pub archives_tsv: Option<PathBuf>,
    pub corpus_report: PathBuf,
    pub split_buckets: Vec<PathBuf>,
    pub embedded_files: Vec<PathBuf>,
    pub app_libraries: Vec<(String, PathBuf)>,
}

pub fn check_rel_path(path: &str) -> Result<()> {
    validate_portable_rel_path(path).map_err(|e| SeedError::Asset(e.to_string()))
}

pub fn parse_stack_key_hex(text: &str) -> Result<u64> {
    if text.len() != 16 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(SeedError::Asset(format!("invalid stack key {text:?}")));
    }
    u64::from_str_radix(text, 16).map_err(|_| SeedError::Asset(format!("invalid stack key {text:?}")))
}

fn is_hex64(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn read_bundle_stack_key(info_plist_bytes: &[u8]) -> Result<Option<String>> {
    let val = plist::Value::from_reader(Cursor::new(info_plist_bytes))?;
    Ok(val
        .as_dictionary()
        .and_then(|d| d.get("SwitchShaderStack"))
        .and_then(plist::Value::as_string)
        .map(str::to_owned))
}

pub fn is_seed_asset_tar(path: &Path) -> Result<bool> {
    if !path.is_file() {
        return Ok(false);
    }
    let file = File::open(path).map_err(|e| SeedError::io_path(path, e))?;
    let mut archive = tar::Archive::new(BufReader::new(file));
    let Ok(entries) = archive.entries() else {
        return Ok(false);
    };
    for entry in entries {
        let Ok(entry) = entry else {
            return Ok(false);
        };
        if !entry.header().entry_type().is_file() {
            continue;
        }
        let Ok(entry_path) = entry.path() else {
            continue;
        };
        if entry_path.as_os_str() == "manifest.json" {
            if entry.size() > MAX_MANIFEST_BYTES {
                return Ok(false);
            }
            let mut bytes = Vec::new();
            if entry.take(MAX_MANIFEST_BYTES + 1).read_to_end(&mut bytes).is_err()
                || bytes.len() as u64 > MAX_MANIFEST_BYTES
            {
                return Ok(false);
            }
            return Ok(serde_json::from_slice::<SeedAssetManifest>(&bytes).is_ok_and(|m| m.format == MANIFEST_FORMAT));
        }
    }
    Ok(false)
}

pub fn read_seed_asset_manifest(tar_path: &Path) -> Result<SeedAssetManifest> {
    let bytes = read_manifest_from_tar(tar_path)?;
    let manifest: SeedAssetManifest = serde_json::from_slice(&bytes)?;
    if manifest.format != MANIFEST_FORMAT {
        return Err(SeedError::Asset(format!(
            "unsupported seed asset manifest format {}",
            manifest.format
        )));
    }
    Ok(manifest)
}

fn read_manifest_from_tar(tar_path: &Path) -> Result<Vec<u8>> {
    let file = File::open(tar_path).map_err(|e| SeedError::io_path(tar_path, e))?;
    let mut archive = tar::Archive::new(BufReader::new(file));
    let mut found: Option<Vec<u8>> = None;
    for entry in archive.entries()? {
        let entry = entry?;
        if !entry.header().entry_type().is_file() {
            return Err(SeedError::Asset(format!(
                "unexpected non-file tar entry {:?}",
                entry.path().ok()
            )));
        }
        let rel = entry
            .path()?
            .to_str()
            .ok_or_else(|| SeedError::Asset("non-UTF-8 tar entry path".into()))?
            .to_owned();
        if rel == "manifest.json" {
            if found.is_some() {
                return Err(SeedError::Asset("duplicate manifest.json in seed tar".into()));
            }
            if entry.size() > MAX_MANIFEST_BYTES {
                return Err(SeedError::Asset("manifest.json exceeds size cap".into()));
            }
            let mut buf = Vec::new();
            entry.take(MAX_MANIFEST_BYTES + 1).read_to_end(&mut buf)?;
            if buf.len() as u64 > MAX_MANIFEST_BYTES {
                return Err(SeedError::Asset("manifest.json exceeds size cap".into()));
            }
            found = Some(buf);
        }
    }
    found.ok_or_else(|| SeedError::Asset("seed asset tar missing manifest.json".into()))
}

fn verify_cached_unpack(unpack_dir: &Path, manifest: &SeedAssetManifest, manifest_sha: &str) -> bool {
    let Ok(dir_meta) = fs::symlink_metadata(unpack_dir) else {
        return false;
    };
    if dir_meta.file_type().is_symlink() || !dir_meta.is_dir() {
        return false;
    }
    let stamp_path = unpack_dir.join(".unpacked.sha256");
    if fs::symlink_metadata(&stamp_path).is_err_and(|_| true)
        || fs::symlink_metadata(&stamp_path).is_ok_and(|m| m.file_type().is_symlink() || !m.is_file())
    {
        return false;
    }
    if fs::read_to_string(&stamp_path)
        .ok()
        .is_none_or(|s| s.trim() != manifest_sha)
    {
        return false;
    }
    if !verify_regular_file_digest(&unpack_dir.join("manifest.json"), None, manifest_sha) {
        return false;
    }
    for spec in &manifest.files {
        if !verify_regular_file_digest(&unpack_dir.join(&spec.path), Some(spec.size), &spec.sha256) {
            return false;
        }
    }
    true
}

fn verify_regular_file_digest(path: &Path, expected_size: Option<u64>, expected_sha: &str) -> bool {
    let Ok(meta) = fs::symlink_metadata(path) else {
        return false;
    };
    if meta.file_type().is_symlink() || !meta.is_file() {
        return false;
    }
    if let Some(sz) = expected_size
        && meta.len() != sz
    {
        return false;
    }
    let Ok(file) = File::open(path) else {
        return false;
    };
    let mut reader = BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let Ok(n) = reader.read(&mut buf) else {
            return false;
        };
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        total += n as u64;
    }
    if let Some(sz) = expected_size
        && total != sz
    {
        return false;
    }
    hex::encode(hasher.finalize()).eq_ignore_ascii_case(expected_sha)
}

fn create_private_temp_dir(target: &Path) -> Result<PathBuf> {
    static SERIAL: AtomicU64 = AtomicU64::new(0);
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
    }
    let base_name = target.file_name().and_then(|s| s.to_str()).unwrap_or("unpacked");
    let seq = SERIAL.fetch_add(1, Ordering::Relaxed);
    let tmp = target.with_file_name(format!(".{base_name}.{}-{seq}.tmp", std::process::id()));
    fs::create_dir(&tmp).map_err(|e| SeedError::io_path(&tmp, e))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(&tmp, fs::Permissions::from_mode(0o700));
    }
    Ok(tmp)
}

fn stream_extract_verified(
    tar_path: &Path,
    manifest: &SeedAssetManifest,
    manifest_sha: &str,
    staging_dir: &Path,
) -> Result<()> {
    let declared: BTreeMap<&str, &SeedAssetFile> = manifest.files.iter().map(|f| (f.path.as_str(), f)).collect();
    let mut seen_lower: BTreeSet<String> = BTreeSet::new();
    let mut extracted_files: BTreeSet<String> = BTreeSet::new();
    let mut total_bytes = 0u64;

    let file = File::open(tar_path).map_err(|e| SeedError::io_path(tar_path, e))?;
    let mut archive = tar::Archive::new(BufReader::new(file));
    let mut buf = [0u8; 64 * 1024];

    for entry in archive.entries()? {
        let mut entry = entry?;
        if !entry.header().entry_type().is_file() {
            return Err(SeedError::Asset(format!(
                "unexpected non-file tar entry {:?}",
                entry.path().ok()
            )));
        }
        let rel = entry
            .path()?
            .to_str()
            .ok_or_else(|| SeedError::Asset("non-UTF-8 tar entry path".into()))?
            .to_owned();
        check_rel_path(&rel)?;
        if !seen_lower.insert(rel.to_ascii_lowercase()) {
            return Err(SeedError::Asset(format!(
                "duplicate or case-colliding tar entry {rel:?}"
            )));
        }
        let is_manifest = rel == "manifest.json";
        let max_len = if is_manifest {
            MAX_MANIFEST_BYTES
        } else {
            MAX_ENTRY_BYTES
        };
        let spec = if is_manifest {
            None
        } else {
            let Some(&spec) = declared.get(rel.as_str()) else {
                return Err(SeedError::Asset(format!("undeclared file {rel:?} in seed tar")));
            };
            Some(spec)
        };
        if entry.size() > max_len {
            return Err(SeedError::Asset(format!("{rel}: entry size exceeds cap")));
        }

        let dest = staging_dir.join(&rel);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
        }
        let mut out = File::create(&dest).map_err(|e| SeedError::io_path(&dest, e))?;
        let mut hasher = Sha256::new();
        let mut written = 0u64;
        loop {
            let n = entry.read(&mut buf)?;
            if n == 0 {
                break;
            }
            written += n as u64;
            total_bytes += n as u64;
            if written > max_len || total_bytes > MAX_TOTAL_BYTES {
                return Err(SeedError::Asset(format!("{rel}: extracted bytes exceed cap")));
            }
            hasher.update(&buf[..n]);
            out.write_all(&buf[..n]).map_err(|e| SeedError::io_path(&dest, e))?;
        }
        out.flush().map_err(|e| SeedError::io_path(&dest, e))?;
        let digest = hex::encode(hasher.finalize());
        if is_manifest {
            if !digest.eq_ignore_ascii_case(manifest_sha) {
                return Err(SeedError::Asset("manifest.json changed while unpacking".into()));
            }
        } else if let Some(spec) = spec {
            if written != spec.size {
                return Err(SeedError::Asset(format!(
                    "{rel}: size {written} != manifest {}",
                    spec.size
                )));
            }
            if !digest.eq_ignore_ascii_case(&spec.sha256) {
                return Err(SeedError::Asset(format!(
                    "{rel}: sha256 {digest} != manifest {}",
                    spec.sha256
                )));
            }
            extracted_files.insert(rel);
        }
    }

    for spec in &manifest.files {
        if !extracted_files.contains(&spec.path) {
            return Err(SeedError::Asset(format!(
                "manifest file {:?} missing from seed tar",
                spec.path
            )));
        }
    }
    fs::write(staging_dir.join(".unpacked.sha256"), format!("{manifest_sha}\n"))
        .map_err(|e| SeedError::io_path(staging_dir, e))?;
    Ok(())
}

pub fn unpack_seed_asset(
    tar_path: &Path,
    expected_build: &str,
    expected_qc_sha256: &str,
    expected_stack_key: Option<&str>,
    unpack_dir: &Path,
) -> Result<UnpackedSeedAsset> {
    let manifest_bytes = read_manifest_from_tar(tar_path)?;
    let manifest_sha = hex::encode(Sha256::digest(&manifest_bytes));
    let mut manifest: SeedAssetManifest = serde_json::from_slice(&manifest_bytes)?;
    validate_manifest(&mut manifest, expected_build, expected_qc_sha256, expected_stack_key)?;

    if !verify_cached_unpack(unpack_dir, &manifest, &manifest_sha) {
        let staging = create_private_temp_dir(unpack_dir)?;
        if let Err(err) = stream_extract_verified(tar_path, &manifest, &manifest_sha, &staging) {
            let _ = fs::remove_dir_all(&staging);
            return Err(err);
        }
        if unpack_dir.exists() || fs::symlink_metadata(unpack_dir).is_ok() {
            let meta = fs::symlink_metadata(unpack_dir).map_err(|e| SeedError::io_path(unpack_dir, e))?;
            if meta.file_type().is_symlink() || meta.is_file() {
                fs::remove_file(unpack_dir).map_err(|e| SeedError::io_path(unpack_dir, e))?;
            } else {
                fs::remove_dir_all(unpack_dir).map_err(|e| SeedError::io_path(unpack_dir, e))?;
            }
        }
        if let Err(err) = fs::rename(&staging, unpack_dir) {
            let _ = fs::remove_dir_all(&staging);
            return Err(SeedError::io_path(unpack_dir, err));
        }
    }

    let split_buckets: Vec<PathBuf> = (0..manifest.bucket_counts.split)
        .map(|i| require_file(unpack_dir, &format!("split/bucket{i}.log")))
        .collect::<Result<_>>()?;
    let embedded_files: Vec<PathBuf> = manifest
        .embedded
        .iter()
        .map(|e| require_file(unpack_dir, &e.path))
        .collect::<Result<_>>()?;
    let app_libraries: Vec<(String, PathBuf)> = manifest
        .app_libraries
        .iter()
        .map(|a| Ok((a.guest_path.clone(), require_file(unpack_dir, &a.path)?)))
        .collect::<Result<_>>()?;
    let archives_tsv = manifest
        .files
        .iter()
        .any(|f| f.path == "raw/archives.tsv")
        .then(|| require_file(unpack_dir, "raw/archives.tsv"))
        .transpose()?;

    Ok(UnpackedSeedAsset {
        manifest,
        root: unpack_dir.to_path_buf(),
        own_metallib: require_file(unpack_dir, "raw/own.metallib")?,
        context_log: require_file(unpack_dir, "raw/context.log")?,
        draw_context: require_file(unpack_dir, "raw/draw_context.txt")?,
        corpus_log: require_file(unpack_dir, "raw/corpus.log")?,
        archives_tsv,
        corpus_report: require_file(unpack_dir, "split/corpus-report.txt")?,
        split_buckets,
        embedded_files,
        app_libraries,
    })
}

fn require_file(root: &Path, rel: &str) -> Result<PathBuf> {
    check_rel_path(rel)?;
    let p = root.join(rel);
    let meta =
        fs::symlink_metadata(&p).map_err(|_| SeedError::Asset(format!("missing required asset file {rel:?}")))?;
    if meta.file_type().is_symlink() || !meta.is_file() {
        return Err(SeedError::Asset(format!("asset file {rel:?} is not a regular file")));
    }
    Ok(p)
}

fn validate_manifest(
    m: &mut SeedAssetManifest,
    expected_build: &str,
    expected_qc_sha256: &str,
    expected_stack_key: Option<&str>,
) -> Result<()> {
    if m.format != MANIFEST_FORMAT {
        return Err(SeedError::Asset(format!(
            "unsupported seed asset manifest format {} (expected {MANIFEST_FORMAT})",
            m.format
        )));
    }
    if m.build != expected_build {
        return Err(SeedError::BuildMismatch {
            got: m.build.clone(),
            expected: expected_build.to_owned(),
        });
    }
    if m.kind != expected_build || rule_for_build(&m.kind).is_none() {
        return Err(SeedError::Asset(format!(
            "seed asset kind {:?} does not match expected supported build {:?}",
            m.kind, expected_build
        )));
    }
    if !is_hex64(&m.quartzcore_metallib_sha256)
        || !m.quartzcore_metallib_sha256.eq_ignore_ascii_case(expected_qc_sha256)
    {
        return Err(SeedError::QuartzCoreMismatch {
            got: m.quartzcore_metallib_sha256.clone(),
            expected: expected_qc_sha256.to_owned(),
        });
    }
    m.quartzcore_metallib_sha256 = m.quartzcore_metallib_sha256.to_ascii_lowercase();
    let stack_u64 = parse_stack_key_hex(&m.stack_key)?;
    m.stack_key = format!("{stack_u64:016x}");
    if let Some(bundle_stack) = expected_stack_key {
        let bundle_u64 = parse_stack_key_hex(bundle_stack)?;
        if bundle_u64 != stack_u64 {
            return Err(SeedError::StackKeyMismatch {
                bundle: format!("{bundle_u64:016x}"),
                asset: m.stack_key.clone(),
            });
        }
    }
    if m.bucket_counts.split != EXPECTED_SPLIT_BUCKETS {
        return Err(SeedError::Asset(format!(
            "expected {EXPECTED_SPLIT_BUCKETS} split buckets, got {}",
            m.bucket_counts.split
        )));
    }

    let mut by_path: BTreeMap<&str, (&str, u64)> = BTreeMap::new();
    let mut seen_lower: BTreeSet<String> = BTreeSet::new();
    seen_lower.insert("manifest.json".to_owned());
    for f in &m.files {
        check_rel_path(&f.path)?;
        if !is_hex64(&f.sha256) {
            return Err(SeedError::Asset(format!("{}: invalid sha256 {:?}", f.path, f.sha256)));
        }
        if !seen_lower.insert(f.path.to_ascii_lowercase()) {
            return Err(SeedError::Asset(format!(
                "duplicate or case-colliding manifest path {:?}",
                f.path
            )));
        }
        by_path.insert(f.path.as_str(), (f.sha256.as_str(), f.size));
    }

    for emb in &m.embedded {
        check_rel_path(&emb.path)?;
        if !is_hex64(&emb.sha256) {
            return Err(SeedError::Asset(format!(
                "embedded entry {:?} has invalid sha256 {:?}",
                emb.path, emb.sha256
            )));
        }
        let expected_path = format!("raw/embedded/{}.metallib", emb.sha256.to_ascii_lowercase());
        if emb.path != expected_path {
            return Err(SeedError::Asset(format!(
                "embedded entry {:?} does not match canonical digest path {:?}",
                emb.path, expected_path
            )));
        }
        let Some(&(inv_sha, inv_size)) = by_path.get(emb.path.as_str()) else {
            return Err(SeedError::Asset(format!(
                "embedded entry {:?} is not in manifest.files",
                emb.path
            )));
        };
        if inv_size != emb.size || !inv_sha.eq_ignore_ascii_case(&emb.sha256) {
            return Err(SeedError::Asset(format!(
                "embedded entry {:?} disagrees with manifest.files",
                emb.path
            )));
        }
    }

    let mut seen_app_guests: BTreeSet<String> = BTreeSet::new();
    for app in &m.app_libraries {
        check_rel_path(&app.guest_path)?;
        if !app.guest_path.starts_with("Applications/") || !app.guest_path.ends_with(".metallib") {
            return Err(SeedError::Asset(format!(
                "app library guest path {:?} must be under Applications/*.metallib",
                app.guest_path
            )));
        }
        if !seen_app_guests.insert(app.guest_path.to_ascii_lowercase()) {
            return Err(SeedError::Asset(format!(
                "duplicate or case-colliding app library guest path {:?}",
                app.guest_path
            )));
        }
        check_rel_path(&app.path)?;
        if !is_hex64(&app.sha256) {
            return Err(SeedError::Asset(format!(
                "app library entry {:?} has invalid sha256 {:?}",
                app.path, app.sha256
            )));
        }
        let expected_path = format!("raw/apps/{}.metallib", app.sha256.to_ascii_lowercase());
        if app.path != expected_path {
            return Err(SeedError::Asset(format!(
                "app library entry {:?} does not match canonical digest path {:?}",
                app.path, expected_path
            )));
        }
        let Some(&(inv_sha, inv_size)) = by_path.get(app.path.as_str()) else {
            return Err(SeedError::Asset(format!(
                "app library entry {:?} is not in manifest.files",
                app.path
            )));
        };
        if inv_size != app.size || !inv_sha.eq_ignore_ascii_case(&app.sha256) {
            return Err(SeedError::Asset(format!(
                "app library entry {:?} disagrees with manifest.files",
                app.path
            )));
        }
    }

    for req in [
        "raw/own.metallib",
        "raw/context.log",
        "raw/draw_context.txt",
        "raw/corpus.log",
        "split/corpus-report.txt",
    ] {
        if !by_path.contains_key(req) {
            return Err(SeedError::Asset(format!("manifest missing required file {req:?}")));
        }
    }
    for i in 0..EXPECTED_SPLIT_BUCKETS {
        let name = format!("split/bucket{i}.log");
        if !by_path.contains_key(name.as_str()) {
            return Err(SeedError::Asset(format!("manifest missing split bucket {name:?}")));
        }
    }
    Ok(())
}
