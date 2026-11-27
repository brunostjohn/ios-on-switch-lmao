// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use plist::{Dictionary, Integer, Value as PlistValue};
use sha2::{Digest, Sha256};

use crate::Error;

pub const HEADER: &str = "# gpu stage 2 ";
pub const KEXT_PREFIX: &str = "# kext ";
pub const TREE_MANIFEST: &str = "gpu.stage";
pub const ADDONS_DIR: &str = "ngpu/addons";
pub const BUNDLE_DIR: &str = "ngpu/addons/TegraGM20BMetal.bundle";
pub const REQUIRED_PLUGIN_PATH: &str = "ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal";
pub const STATE_PATH: &str = "ngpu/stage.tsv.gz";
pub const ICON_CACHE_RECORD_PATH: &str = "Switch/icon-cache-gpu";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ManifestForm {
    Tree,
    Runfiles(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ManifestFile {
    pub path: String,
    pub sha256: [u8; 32],
    pub size: u64,
    pub mode: u16,
    pub source: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PrefFile {
    pub owner_uid: u32,
    pub values: BTreeMap<String, PlistValue>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ParsedManifest {
    pub form: ManifestForm,
    pub kext: Option<String>,
    pub files: BTreeMap<String, ManifestFile>,
    pub prefs: BTreeMap<String, PrefFile>,
}

#[derive(Clone, Debug)]
pub enum FileContent {
    Path(PathBuf),
    Bytes(Arc<[u8]>),
}

#[derive(Clone, Debug)]
pub struct FileRecord {
    pub path: String,
    pub sha256: [u8; 32],
    pub size: u64,
    pub mode: u16,
    pub source: String,
    pub content: FileContent,
}

#[derive(Clone, Debug)]
pub struct GpuStage {
    pub kext: Option<String>,
    pub files: BTreeMap<String, FileRecord>,
    pub prefs: BTreeMap<String, PrefFile>,
}

pub fn check_path(path: &str) -> Result<&str, Error> {
    if path.is_empty()
        || path.starts_with('/')
        || path.contains('\0')
        || path.split('/').any(|p| p.is_empty() || p == "." || p == "..")
    {
        return Err(Error::InvalidPath(path.to_owned()));
    }
    Ok(path)
}

pub fn json_to_plist(value: serde_json::Value) -> Result<PlistValue, Error> {
    match value {
        serde_json::Value::Null => Err(Error::NullPrefValue),
        serde_json::Value::Bool(b) => Ok(PlistValue::Boolean(b)),
        serde_json::Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                Ok(PlistValue::Integer(Integer::from(i)))
            } else if let Some(u) = n.as_u64() {
                Ok(PlistValue::Integer(Integer::from(u)))
            } else if let Some(f) = n.as_f64().filter(|f| f.is_finite()) {
                Ok(PlistValue::Real(f))
            } else {
                Err(Error::Manifest(format!("unsupported JSON number {n}")))
            }
        }
        serde_json::Value::String(s) => Ok(PlistValue::String(s)),
        serde_json::Value::Array(items) => {
            let mut out = Vec::with_capacity(items.len());
            for item in items {
                out.push(json_to_plist(item)?);
            }
            Ok(PlistValue::Array(out))
        }
        serde_json::Value::Object(map) => {
            let mut dict = Dictionary::new();
            for (k, v) in map {
                dict.insert(k, json_to_plist(v)?);
            }
            Ok(PlistValue::Dictionary(dict))
        }
    }
}

impl ParsedManifest {
    pub fn parse(text: &str) -> Result<Self, Error> {
        let mut lines = text.lines();
        let header = lines
            .next()
            .ok_or_else(|| Error::Manifest("empty manifest; expected format 2 header".into()))?;
        let Some(rest) = header.strip_prefix(HEADER) else {
            return Err(Error::Manifest(format!(
                "expected a format 2 manifest header ({HEADER:?}), got {header:?}"
            )));
        };
        let form = match rest.split_once(' ') {
            None if rest == "tree" => ManifestForm::Tree,
            Some(("runfiles", own)) if !own.is_empty() => {
                check_path(own)?;
                ManifestForm::Runfiles(own.to_owned())
            }
            _ => {
                return Err(Error::Manifest(format!("unsupported format 2 header {header:?}")));
            }
        };

        let mut kext: Option<String> = None;
        let mut files = BTreeMap::new();
        let mut prefs: BTreeMap<String, PrefFile> = BTreeMap::new();

        for (idx, line) in lines.enumerate() {
            let line_no = idx + 2;
            if let Some(digest) = line.strip_prefix(KEXT_PREFIX) {
                if digest.is_empty() || digest.chars().any(char::is_whitespace) {
                    return Err(Error::Manifest(format!(
                        "line {line_no}: empty or malformed # kext digest"
                    )));
                }
                if let Some(existing) = &kext {
                    if existing != digest {
                        return Err(Error::Manifest(format!(
                            "line {line_no}: conflicting # kext digest ({existing:?} vs {digest:?})"
                        )));
                    }
                } else {
                    kext = Some(digest.to_owned());
                }
                continue;
            }

            let fields: Vec<&str> = line.split('\t').collect();
            match fields.as_slice() {
                ["file", path, sha_str, size_str, mode_str, source] => {
                    check_path(path)?;
                    check_path(source)?;
                    if files.contains_key(*path) {
                        return Err(Error::DuplicateFile((*path).to_owned()));
                    }
                    let sha256 = parse_sha256_hex(sha_str, line_no)?;
                    let size = parse_u64(size_str, "size", line_no)?;
                    let mode = parse_mode_octal(mode_str, line_no)?;
                    files.insert(
                        (*path).to_owned(),
                        ManifestFile {
                            path: (*path).to_owned(),
                            sha256,
                            size,
                            mode,
                            source: (*source).to_owned(),
                        },
                    );
                }
                ["pref", plist_path, uid_str, key, value_json] => {
                    check_path(plist_path)?;
                    let owner_uid = parse_u32(uid_str, "owner uid", line_no)?;
                    let raw: serde_json::Value = serde_json::from_str(value_json)
                        .map_err(|e| Error::Manifest(format!("line {line_no}: invalid pref JSON: {e}")))?;
                    let value = json_to_plist(raw)?;
                    let entry = prefs.entry((*plist_path).to_owned()).or_insert_with(|| PrefFile {
                        owner_uid,
                        values: BTreeMap::new(),
                    });
                    if entry.owner_uid != owner_uid {
                        return Err(Error::ConflictingPrefOwner {
                            path: (*plist_path).to_owned(),
                            first: entry.owner_uid,
                            second: owner_uid,
                        });
                    }
                    entry.values.insert((*key).to_owned(), value);
                }
                [kind @ ("file" | "pref"), ..] => {
                    return Err(Error::Manifest(format!(
                        "line {line_no}: wrong field count for {kind:?} record ({})",
                        fields.len()
                    )));
                }
                [other, ..] => {
                    return Err(Error::Manifest(format!(
                        "line {line_no}: unknown record kind {other:?}"
                    )));
                }
                [] => {
                    return Err(Error::Manifest(format!("line {line_no}: empty record")));
                }
            }
        }

        if !files.contains_key(REQUIRED_PLUGIN_PATH) {
            return Err(Error::MissingPlugin);
        }

        Ok(Self {
            form,
            kext,
            files,
            prefs,
        })
    }
}

impl GpuStage {
    pub fn from_path(path: &Path) -> Result<Self, Error> {
        let manifest_path = if path.is_dir() {
            let candidate = path.join(TREE_MANIFEST);
            if !candidate.is_file() {
                return Err(Error::MissingTreeManifest(path.to_path_buf()));
            }
            candidate
        } else {
            path.to_path_buf()
        };
        let text = fs::read_to_string(&manifest_path)?;
        Self::parse_manifest(&text, &manifest_path)
    }

    pub fn parse_manifest(text: &str, manifest_path: &Path) -> Result<Self, Error> {
        let parsed = ParsedManifest::parse(text)?;
        let root = match &parsed.form {
            ManifestForm::Tree => manifest_path.parent().map(Path::to_path_buf).unwrap_or_default(),
            ManifestForm::Runfiles(own) => resolve_runfiles_root(manifest_path, own)?,
        };
        let files = parsed
            .files
            .into_iter()
            .map(|(path, f)| {
                let source_path = join_posix(&root, &f.source);
                (
                    path,
                    FileRecord {
                        path: f.path,
                        sha256: f.sha256,
                        size: f.size,
                        mode: f.mode,
                        source: f.source,
                        content: FileContent::Path(source_path),
                    },
                )
            })
            .collect();
        Ok(Self {
            kext: parsed.kext,
            files,
            prefs: parsed.prefs,
        })
    }

    pub fn parse_manifest_memory(text: &str, contents: &BTreeMap<String, Arc<[u8]>>) -> Result<Self, Error> {
        let parsed = ParsedManifest::parse(text)?;
        let mut files = BTreeMap::new();
        for (path, f) in parsed.files {
            let bytes = contents
                .get(&f.source)
                .ok_or_else(|| Error::MissingSourceFile(f.source.clone()))?;
            files.insert(
                path,
                FileRecord {
                    path: f.path,
                    sha256: f.sha256,
                    size: f.size,
                    mode: f.mode,
                    source: f.source,
                    content: FileContent::Bytes(Arc::clone(bytes)),
                },
            );
        }
        Ok(Self {
            kext: parsed.kext,
            files,
            prefs: parsed.prefs,
        })
    }

    pub fn tsv_body(&self) -> String {
        let mut out = String::new();
        for f in self.files.values() {
            out.push_str(&format!(
                "{}\t{}\t{}\t{:o}\n",
                f.path,
                hex::encode(f.sha256),
                f.size,
                f.mode
            ));
        }
        out
    }

    pub fn whole_digest(&self) -> String {
        hex::encode(Sha256::digest(self.tsv_body().as_bytes()))
    }

    pub fn icon_build(&self) -> String {
        let whole = self.whole_digest();
        match &self.kext {
            Some(kext) if !kext.is_empty() => hex::encode(Sha256::digest(format!("{whole}\n{kext}").as_bytes())),
            _ => whole,
        }
    }

    pub fn stage_tsv(&self) -> String {
        let body = self.tsv_body();
        let whole = hex::encode(Sha256::digest(body.as_bytes()));
        format!("# {whole}\n{body}")
    }
}

fn resolve_runfiles_root(manifest_path: &Path, own_rlocation: &str) -> Result<PathBuf, Error> {
    let mut cur = manifest_path;
    for part in own_rlocation.split('/').rev() {
        let matched = cur.file_name().and_then(|n| n.to_str()) == Some(part);
        if !matched {
            return Err(Error::Manifest(format!(
                "{}: path does not end with /{own_rlocation}",
                manifest_path.display()
            )));
        }
        cur = cur.parent().ok_or_else(|| {
            Error::Manifest(format!(
                "{}: path does not end with /{own_rlocation}",
                manifest_path.display()
            ))
        })?;
    }
    // Mirror Python `str(manifest).endswith('/' + own)`: a bare relative path equal to `own` has no leading `/`.
    if cur.as_os_str().is_empty() {
        return Err(Error::Manifest(format!(
            "{}: path does not end with /{own_rlocation}",
            manifest_path.display()
        )));
    }
    Ok(cur.to_path_buf())
}

fn join_posix(root: &Path, rel: &str) -> PathBuf {
    let mut out = root.to_path_buf();
    for part in rel.split('/') {
        out.push(part);
    }
    out
}

fn parse_sha256_hex(text: &str, line_no: usize) -> Result<[u8; 32], Error> {
    if text.len() != 64 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(Error::Manifest(format!("line {line_no}: invalid sha256 {text:?}")));
    }
    let mut out = [0u8; 32];
    hex::decode_to_slice(text, &mut out)
        .map_err(|_| Error::Manifest(format!("line {line_no}: invalid sha256 {text:?}")))?;
    Ok(out)
}

fn parse_u64(text: &str, field: &str, line_no: usize) -> Result<u64, Error> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::Manifest(format!("line {line_no}: invalid {field} {text:?}")));
    }
    text.parse::<u64>()
        .map_err(|_| Error::Manifest(format!("line {line_no}: invalid {field} {text:?}")))
}

fn parse_u32(text: &str, field: &str, line_no: usize) -> Result<u32, Error> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::Manifest(format!("line {line_no}: invalid {field} {text:?}")));
    }
    text.parse::<u32>()
        .map_err(|_| Error::Manifest(format!("line {line_no}: invalid {field} {text:?}")))
}

fn parse_mode_octal(text: &str, line_no: usize) -> Result<u16, Error> {
    if text.is_empty() || !text.bytes().all(|b| (b'0'..=b'7').contains(&b)) {
        return Err(Error::Manifest(format!("line {line_no}: invalid octal mode {text:?}")));
    }
    let mode = u16::from_str_radix(text, 8)
        .map_err(|_| Error::Manifest(format!("line {line_no}: invalid octal mode {text:?}")))?;
    if mode > 0o7777 {
        return Err(Error::Manifest(format!(
            "line {line_no}: mode {mode:#o} exceeds 0o7777"
        )));
    }
    Ok(mode)
}
