// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io::Read;
use std::path::PathBuf;
use std::sync::Arc;

use tar::{Archive, EntryType};

use crate::Error;
use crate::manifest::{FileContent, FileRecord, GpuStage, ManifestForm, ParsedManifest, TREE_MANIFEST, check_path};

impl GpuStage {
    pub fn from_tar<R: Read>(reader: R) -> Result<Self, Error> {
        let mut archive = Archive::new(reader);
        let mut entries: BTreeMap<String, Arc<[u8]>> = BTreeMap::new();

        for item in archive.entries()? {
            let mut entry = item?;
            let kind = entry.header().entry_type();
            if kind == EntryType::Directory {
                continue;
            }
            if kind != EntryType::Regular && kind != EntryType::Continuous {
                return Err(Error::Manifest(format!("unsupported tar entry type {kind:?}")));
            }
            let raw_path = entry
                .path()
                .map_err(|e| Error::Manifest(format!("invalid tar entry path: {e}")))?;
            let path_str = raw_path
                .to_str()
                .ok_or_else(|| Error::Manifest("non-UTF-8 tar entry path".into()))?
                .replace('\\', "/");
            let mut norm = path_str.as_str();
            while let Some(stripped) = norm.strip_prefix("./") {
                norm = stripped;
            }
            check_path(norm)?;
            let mut buf = Vec::new();
            entry.read_to_end(&mut buf)?;
            if entries.insert(norm.to_owned(), Arc::from(buf)).is_some() {
                return Err(Error::DuplicateFile(norm.to_owned()));
            }
        }

        let (manifest_key, tree_prefix) = find_tar_manifest(&entries)?;
        let manifest_bytes = entries
            .get(&manifest_key)
            .ok_or_else(|| Error::MissingTreeManifest(PathBuf::from(TREE_MANIFEST)))?;
        let text = std::str::from_utf8(manifest_bytes)
            .map_err(|e| Error::Manifest(format!("{manifest_key}: not UTF-8: {e}")))?;
        let parsed = ParsedManifest::parse(text)?;

        let root_prefix = match &parsed.form {
            ManifestForm::Tree => tree_prefix,
            ManifestForm::Runfiles(own) => {
                if manifest_key == *own {
                    String::new()
                } else if let Some(prefix) = manifest_key.strip_suffix(&format!("/{own}")) {
                    format!("{prefix}/")
                } else {
                    return Err(Error::Manifest(format!(
                        "{manifest_key}: runfiles manifest path does not end with /{own}"
                    )));
                }
            }
        };

        let mut files = BTreeMap::new();
        for (path, f) in parsed.files {
            let lookup = format!("{root_prefix}{}", f.source);
            let bytes = entries
                .get(&lookup)
                .ok_or_else(|| Error::MissingSourceFile(lookup.clone()))?;
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
}

fn find_tar_manifest(entries: &BTreeMap<String, Arc<[u8]>>) -> Result<(String, String), Error> {
    if entries.contains_key(TREE_MANIFEST) {
        return Ok((TREE_MANIFEST.to_owned(), String::new()));
    }
    let suffix = format!("/{TREE_MANIFEST}");
    let mut matches: Vec<&String> = entries.keys().filter(|k| k.ends_with(&suffix)).collect();
    match matches.len() {
        0 => Err(Error::MissingTreeManifest(PathBuf::from(TREE_MANIFEST))),
        1 => {
            let key = matches.remove(0).clone();
            let prefix = key[..key.len() - TREE_MANIFEST.len()].to_owned();
            Ok((key, prefix))
        }
        _ => Err(Error::Manifest(format!(
            "tar archive contains multiple {TREE_MANIFEST} manifests"
        ))),
    }
}
