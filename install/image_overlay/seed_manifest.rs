// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::Arc;

use serde::Deserialize;
use sha2::{Digest, Sha256};
use switch_apfs_write::{Content, InodeRef, Kind, Tree};

use crate::Error;
use crate::source::{SourceKind, TreeSource, is_apple_double};

pub const APP: &str = "SwitchInstaller.app";
pub const IDENTIFIER: &str = "com.brunostjohn.switch.installer";
pub const MANIFEST: &str = "migration/installer-seed.json";
pub const BUILTIN_MANIFEST: &str = "installer-bundle.json";

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
pub struct SeedManifest {
    pub schema: u64,
    pub identifier: String,
    #[serde(default)]
    pub version: Option<String>,
    pub files: BTreeMap<String, String>,
}

pub fn parse_seed_manifest(bytes: &[u8]) -> Result<SeedManifest, Error> {
    let pin: SeedManifest =
        serde_json::from_slice(bytes).map_err(|e| Error::SeedPin(format!("invalid Installer manifest: {e}")))?;
    validate_manifest(&pin)?;
    Ok(pin)
}

fn validate_manifest(pin: &SeedManifest) -> Result<(), Error> {
    if pin.schema != 1 || pin.identifier != IDENTIFIER {
        return Err(Error::SeedPin("invalid Installer seed or manifest".into()));
    }
    for name in pin.files.keys() {
        if name.contains('\\') || name.contains('\0') || name.split('/').any(|p| p.is_empty() || p == "." || p == "..")
        {
            return Err(Error::SeedPin(format!("unsafe Installer bundle member: {name:?}")));
        }
    }
    Ok(())
}

pub(crate) enum CheckItem<'a> {
    Dir,
    File(&'a [u8]),
    Other,
}

pub(crate) fn verify_items<'a>(
    pin: &SeedManifest,
    items: impl IntoIterator<Item = (String, CheckItem<'a>)>,
) -> Result<(), Error> {
    validate_manifest(pin)?;
    let mut allowed_dirs: BTreeSet<String> = BTreeSet::new();
    for name in pin.files.keys() {
        let mut cursor = name.as_str();
        while let Some((parent, _)) = cursor.rsplit_once('/') {
            allowed_dirs.insert(parent.to_owned());
            cursor = parent;
        }
    }
    let mut seen: BTreeSet<String> = BTreeSet::new();
    for (rel, item) in items {
        match item {
            CheckItem::Dir if allowed_dirs.contains(&rel) => continue,
            CheckItem::File(bytes) => {
                let Some(expected) = pin.files.get(&rel) else {
                    return Err(Error::SeedPin(format!(
                        "Installer seed differs from the image input: {rel}"
                    )));
                };
                let got = hex::encode(Sha256::digest(bytes));
                if &got != expected {
                    return Err(Error::SeedPin(format!(
                        "Installer seed differs from the image input: {rel}"
                    )));
                }
                seen.insert(rel);
            }
            _ => {
                return Err(Error::SeedPin(format!(
                    "Installer seed differs from the image input: {rel}"
                )));
            }
        }
    }
    let missing: Vec<&String> = pin.files.keys().filter(|k| !seen.contains(*k)).collect();
    if !missing.is_empty() {
        return Err(Error::SeedPin(format!("Installer seed missing files: {missing:?}")));
    }
    Ok(())
}

pub(crate) fn verify_source_app(
    source: &TreeSource,
    app_prefix: &str,
    pin: &SeedManifest,
    skip_apple_double: bool,
) -> Result<(), Error> {
    let prefix_slash = format!("{app_prefix}/");
    let has_dir_entry = matches!(source.get(app_prefix).map(|e| &e.kind), Some(SourceKind::Dir));
    let has_non_dir_entry = source.get(app_prefix).is_some_and(|e| e.kind != SourceKind::Dir);
    if has_non_dir_entry {
        return Err(Error::SeedPin(format!(
            "invalid Installer seed or manifest: {app_prefix}"
        )));
    }

    let mut items = Vec::new();
    for (path, entry) in source.entries() {
        if skip_apple_double && is_apple_double(path) {
            continue;
        }
        let Some(rel) = path.strip_prefix(&prefix_slash) else {
            continue;
        };
        let item = match &entry.kind {
            SourceKind::Dir => CheckItem::Dir,
            SourceKind::File(bytes) => CheckItem::File(bytes.as_ref()),
            SourceKind::Symlink(_) => CheckItem::Other,
        };
        items.push((rel.to_owned(), item));
    }
    if !has_dir_entry && items.is_empty() {
        return Err(Error::SeedPin(format!(
            "invalid Installer seed or manifest: {app_prefix}"
        )));
    }
    verify_items(pin, items)
}

pub(crate) fn verify_tree_app(
    tree: &Tree,
    children: &HashMap<InodeRef, Vec<(String, InodeRef)>>,
    app_inode: InodeRef,
    pin: &SeedManifest,
) -> Result<(), Error> {
    if tree.inode(app_inode).kind != Kind::Dir {
        return Err(Error::SeedPin("invalid Installer seed or manifest".into()));
    }
    let mut collected: Vec<(String, Option<Arc<[u8]>>, Kind)> = Vec::new();
    collect_tree_entries(tree, children, app_inode, "", &mut collected)?;
    let items = collected.iter().map(|(rel, bytes, kind)| {
        let item = match kind {
            Kind::Dir => CheckItem::Dir,
            Kind::File => CheckItem::File(bytes.as_deref().unwrap_or(&[])),
            Kind::Symlink => CheckItem::Other,
        };
        (rel.clone(), item)
    });
    verify_items(pin, items)
}

fn collect_tree_entries(
    tree: &Tree,
    children: &HashMap<InodeRef, Vec<(String, InodeRef)>>,
    dir: InodeRef,
    prefix: &str,
    out: &mut Vec<(String, Option<Arc<[u8]>>, Kind)>,
) -> Result<(), Error> {
    let Some(list) = children.get(&dir) else {
        return Ok(());
    };
    for (name, child) in list {
        let rel = if prefix.is_empty() {
            name.clone()
        } else {
            format!("{prefix}/{name}")
        };
        let node = tree.inode(*child);
        match node.kind {
            Kind::Dir => {
                out.push((rel.clone(), None, Kind::Dir));
                collect_tree_entries(tree, children, *child, &rel, out)?;
            }
            Kind::File => {
                let bytes = match node.data {
                    None => None,
                    Some(stream) => match &tree.stream(stream).content {
                        Content::Bytes(b) => Some(Arc::clone(b)),
                        _ => {
                            return Err(Error::SeedPin(format!(
                                "{rel}: Installer seed file content is not in memory"
                            )));
                        }
                    },
                };
                out.push((rel, bytes, Kind::File));
            }
            Kind::Symlink => out.push((rel, None, Kind::Symlink)),
        }
    }
    Ok(())
}
