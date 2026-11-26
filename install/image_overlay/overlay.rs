// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_write::{Attrs, InodeRef, Kind, Tree};

use crate::Error;
use crate::seed_manifest::{APP, parse_seed_manifest, verify_source_app};
use crate::source::{SourceKind, TreeSource, is_apple_double, validate_ancestors};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OverlayReport {
    pub dirs: usize,
    pub files: usize,
    pub symlinks: usize,
    pub skipped_apple_double: usize,
    pub builtin_pin_verified: bool,
}

fn overlay_file_mode(mode: u16) -> u16 {
    (mode & 0o777) | 0o200
}

fn overlay_dir_mode(mode: u16) -> u16 {
    (mode & 0o777) | 0o755
}

fn overlay_symlink_mode(mode: u16) -> u16 {
    mode & 0o777
}

fn builtin_app_present(source: &TreeSource) -> bool {
    let app_path = format!("Applications/{APP}");
    let app_prefix = format!("{app_path}/");
    source
        .entries()
        .keys()
        .filter(|path| !is_apple_double(path))
        .any(|path| path == &app_path || path.starts_with(&app_prefix))
}

/// Places an overlay tree under `Switch/overlay` on `tree`, porting `tools/install/card.py:sync_overlay`.
pub fn place_overlay(tree: &mut Tree, source: &TreeSource, builtin_pin: Option<&[u8]>) -> Result<OverlayReport, Error> {
    validate_ancestors(source.entries())?;

    let root = tree.root();
    if let Some(switch_inode) = tree.child(root, "Switch") {
        if tree.inode(switch_inode).kind != Kind::Dir {
            return Err(Error::Source("Switch is not a directory".into()));
        }
        if tree.child(switch_inode, "overlay").is_some() {
            return Err(Error::Source("Switch/overlay already exists".into()));
        }
    }

    let has_builtin_app = builtin_app_present(source);
    let builtin_pin_verified = match (builtin_pin, has_builtin_app) {
        (None, false) => false,
        (None, true) => {
            return Err(Error::BuiltinPin(
                "cannot verify built-in Installer: missing installer-bundle.json".into(),
            ));
        }
        (Some(bytes), _) => {
            let pin = parse_seed_manifest(bytes).map_err(|e| Error::BuiltinPin(e.to_string()))?;
            let app_path = format!("Applications/{APP}");
            verify_source_app(source, &app_path, &pin, true).map_err(|e| Error::BuiltinPin(e.to_string()))?;
            true
        }
    };

    let mut report = OverlayReport {
        builtin_pin_verified,
        ..Default::default()
    };

    let mut ordered: Vec<(&String, &crate::source::SourceEntry)> = Vec::new();
    for (path, entry) in source.entries() {
        if is_apple_double(path) {
            report.skipped_apple_double += 1;
            continue;
        }
        ordered.push((path, entry));
    }
    ordered.sort_by(|(a, _), (b, _)| {
        a.matches('/')
            .count()
            .cmp(&b.matches('/').count())
            .then_with(|| a.cmp(b))
    });

    let switch_dir = match tree.child(root, "Switch") {
        Some(dir) => dir,
        None => tree.add_dir(root, "Switch", Attrs::new(0, 0, 0o755))?,
    };
    let overlay_dir = tree.add_dir(
        switch_dir,
        "overlay",
        Attrs::new(0, 0, overlay_dir_mode(source.root_mode())),
    )?;

    let mut dirs: HashMap<String, InodeRef> = HashMap::new();
    dirs.insert(String::new(), overlay_dir);

    for (path, entry) in ordered {
        let (parent_path, name) = match path.rsplit_once('/') {
            Some((p, n)) => (p, n),
            None => ("", path.as_str()),
        };
        let parent = ensure_overlay_dir(tree, &mut dirs, parent_path, &mut report.dirs)?;
        match &entry.kind {
            SourceKind::Dir => {
                let mode = overlay_dir_mode(entry.mode);
                if let Some(&existing) = dirs.get(path.as_str()) {
                    tree.inode_mut(existing).attrs = Attrs::new(0, 0, mode);
                } else {
                    let inode = tree.add_dir(parent, name, Attrs::new(0, 0, mode))?;
                    dirs.insert(path.clone(), inode);
                    report.dirs += 1;
                }
            }
            SourceKind::File(bytes) => {
                let stream = if bytes.is_empty() {
                    None
                } else {
                    Some(tree.add_bytes(Arc::clone(bytes)))
                };
                tree.add_file(parent, name, Attrs::new(0, 0, overlay_file_mode(entry.mode)), stream)?;
                report.files += 1;
            }
            SourceKind::Symlink(target) => {
                tree.add_symlink(parent, name, Attrs::new(0, 0, overlay_symlink_mode(entry.mode)), target)?;
                report.symlinks += 1;
            }
        }
    }

    Ok(report)
}

fn ensure_overlay_dir(
    tree: &mut Tree,
    dirs: &mut HashMap<String, InodeRef>,
    rel: &str,
    dir_count: &mut usize,
) -> Result<InodeRef, Error> {
    if let Some(&inode) = dirs.get(rel) {
        return Ok(inode);
    }
    let (parent_path, name) = match rel.rsplit_once('/') {
        Some((p, n)) => (p, n),
        None => ("", rel),
    };
    let parent = ensure_overlay_dir(tree, dirs, parent_path, dir_count)?;
    let inode = tree.add_dir(parent, name, Attrs::new(0, 0, 0o755))?;
    dirs.insert(rel.to_owned(), inode);
    *dir_count += 1;
    Ok(inode)
}

/// Deterministic SHA-256 digest matching `tools/install/card.py:tree_digest`.
pub fn tree_digest(source: &TreeSource) -> String {
    let mut hasher = Sha256::new();
    for (path, entry) in source.entries() {
        if is_apple_double(path) {
            continue;
        }
        match &entry.kind {
            SourceKind::Dir => {
                hasher.update(b"d ");
                hasher.update(path.as_bytes());
                hasher.update(b"\n");
            }
            SourceKind::File(bytes) => {
                let file_hash = hex::encode(Sha256::digest(bytes.as_ref()));
                let mode = overlay_file_mode(entry.mode);
                hasher.update(format!("f {path} {mode:o} {file_hash}\n").as_bytes());
            }
            SourceKind::Symlink(target) => {
                let target_hash = hex::encode(Sha256::digest(target.as_bytes()));
                let mode = overlay_file_mode(entry.mode);
                hasher.update(format!("f {path} {mode:o} {target_hash}\n").as_bytes());
            }
        }
    }
    hex::encode(hasher.finalize())
}
