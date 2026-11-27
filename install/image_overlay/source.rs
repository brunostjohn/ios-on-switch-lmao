// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::Path;
use std::sync::Arc;

use crate::Error;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceKind {
    Dir,
    File(Arc<[u8]>),
    Symlink(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceEntry {
    pub kind: SourceKind,
    pub mode: u16,
}

#[derive(Clone, Debug)]
pub struct TreeSource {
    root_mode: u16,
    entries: BTreeMap<String, SourceEntry>,
}

impl Default for TreeSource {
    fn default() -> Self {
        Self::new()
    }
}

impl TreeSource {
    pub fn new() -> Self {
        Self {
            root_mode: 0o755,
            entries: BTreeMap::new(),
        }
    }

    pub fn with_root_mode(mut self, mode: u16) -> Self {
        self.root_mode = mode & 0o7777;
        self
    }

    pub fn root_mode(&self) -> u16 {
        self.root_mode
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn entries(&self) -> &BTreeMap<String, SourceEntry> {
        &self.entries
    }

    pub fn get(&self, path: &str) -> Option<&SourceEntry> {
        self.entries.get(path)
    }

    pub fn add_dir(&mut self, path: &str, mode: u16) -> Result<(), Error> {
        self.insert(
            path,
            SourceEntry {
                kind: SourceKind::Dir,
                mode: mode & 0o7777,
            },
        )
    }

    pub fn add_file(&mut self, path: &str, bytes: impl Into<Arc<[u8]>>, mode: u16) -> Result<(), Error> {
        self.insert(
            path,
            SourceEntry {
                kind: SourceKind::File(bytes.into()),
                mode: mode & 0o7777,
            },
        )
    }

    pub fn add_symlink(&mut self, path: &str, target: &str, mode: u16) -> Result<(), Error> {
        if target.is_empty() || target.contains('\0') {
            return Err(Error::Source(format!("{path}: invalid symlink target")));
        }
        self.insert(
            path,
            SourceEntry {
                kind: SourceKind::Symlink(target.to_owned()),
                mode: mode & 0o7777,
            },
        )
    }

    fn insert(&mut self, path: &str, entry: SourceEntry) -> Result<(), Error> {
        let name = normalize_rel_path(path)?;
        if name.is_empty() {
            return Err(Error::Source("empty entry path".into()));
        }
        if self.entries.contains_key(&name) {
            return Err(Error::Source(format!("duplicate entry {name}")));
        }
        self.entries.insert(name, entry);
        Ok(())
    }

    pub fn from_dir(root: &Path) -> Result<Self, Error> {
        let meta = fs::symlink_metadata(root)?;
        if !meta.file_type().is_dir() {
            return Err(Error::Source(format!("{}: not a directory", root.display())));
        }
        let mut tree = Self::new().with_root_mode(metadata_mode(&meta));
        walk_dir(root, "", &mut tree)?;
        Ok(tree)
    }

    pub fn from_tar(reader: impl Read) -> Result<Self, Error> {
        Self::from_tar_prefix(reader, "")
    }

    pub fn from_tar_prefix(reader: impl Read, prefix: &str) -> Result<Self, Error> {
        let prefix = normalize_prefix(prefix)?;
        let mut tree = Self::new();
        let mut archive = tar::Archive::new(reader);
        for entry in archive.entries()? {
            let mut entry = entry?;
            let raw = entry.path_bytes();
            let raw = std::str::from_utf8(&raw).map_err(|_| Error::Source("tar entry path is not UTF-8".into()))?;
            let normalized = normalize_rel_path(raw)?;
            let kind = entry.header().entry_type();
            let raw_mode = entry.header().mode()?;
            if raw_mode & !0o7777 != 0 {
                return Err(Error::Source(format!("{raw}: invalid tar mode 0o{raw_mode:o}")));
            }
            let mode = raw_mode as u16;
            if normalized.is_empty() {
                if !kind.is_dir() {
                    return Err(Error::Source("tar root is not a directory".into()));
                }
                if prefix.is_empty() {
                    tree.root_mode = mode;
                }
                continue;
            }
            let rel = if prefix.is_empty() {
                normalized
            } else if normalized == prefix {
                if !kind.is_dir() {
                    return Err(Error::Source(format!("{prefix}: prefix is not a directory")));
                }
                tree.root_mode = mode;
                continue;
            } else if let Some(rest) = normalized.strip_prefix(&format!("{prefix}/")) {
                rest.to_owned()
            } else {
                continue;
            };

            let source_kind = if kind.is_dir() {
                SourceKind::Dir
            } else if kind.is_file() {
                let mut buf = Vec::with_capacity(entry.size() as usize);
                entry.read_to_end(&mut buf)?;
                SourceKind::File(Arc::from(buf))
            } else if kind.is_symlink() {
                let target_bytes = entry
                    .link_name_bytes()
                    .ok_or_else(|| Error::Source(format!("{rel}: symlink missing target")))?;
                let target = std::str::from_utf8(&target_bytes)
                    .map_err(|_| Error::Source(format!("{rel}: symlink target is not UTF-8")))?;
                if target.is_empty() || target.contains('\0') {
                    return Err(Error::Source(format!("{rel}: invalid symlink target")));
                }
                SourceKind::Symlink(target.to_owned())
            } else {
                return Err(Error::Source(format!("{rel}: unsupported tar entry type {kind:?}")));
            };

            if tree.entries.contains_key(&rel) {
                return Err(Error::Source(format!("duplicate tar entry {rel}")));
            }
            tree.entries.insert(
                rel,
                SourceEntry {
                    kind: source_kind,
                    mode,
                },
            );
        }
        Ok(tree)
    }
}

fn walk_dir(dir: &Path, rel_prefix: &str, out: &mut TreeSource) -> Result<(), Error> {
    let mut names = Vec::new();
    for item in fs::read_dir(dir)? {
        let item = item?;
        let name = item
            .file_name()
            .into_string()
            .map_err(|_| Error::Source(format!("{}: non-UTF-8 file name", item.path().display())))?;
        if name.contains('/') || name.contains('\0') {
            return Err(Error::Source(format!("unsafe file name {name:?}")));
        }
        names.push((name, item.path()));
    }
    names.sort_by(|a, b| a.0.cmp(&b.0));

    for (name, path) in names {
        let rel = if rel_prefix.is_empty() {
            name
        } else {
            format!("{rel_prefix}/{name}")
        };
        let meta = fs::symlink_metadata(&path)?;
        let ft = meta.file_type();
        let mode = metadata_mode(&meta);
        if ft.is_dir() {
            out.add_dir(&rel, mode)?;
            walk_dir(&path, &rel, out)?;
        } else if ft.is_file() {
            let bytes = fs::read(&path)?;
            out.add_file(&rel, bytes, mode)?;
        } else if ft.is_symlink() {
            let target = fs::read_link(&path)?;
            let target = target
                .to_str()
                .ok_or_else(|| Error::Source(format!("{rel}: non-UTF-8 symlink target")))?;
            out.add_symlink(&rel, target, mode)?;
        } else {
            return Err(Error::Source(format!("{}: unsupported special file", path.display())));
        }
    }
    Ok(())
}

#[cfg(unix)]
fn metadata_mode(meta: &fs::Metadata) -> u16 {
    use std::os::unix::fs::PermissionsExt;
    (meta.permissions().mode() & 0o7777) as u16
}

#[cfg(not(unix))]
fn metadata_mode(meta: &fs::Metadata) -> u16 {
    let ft = meta.file_type();
    if ft.is_dir() {
        0o755
    } else if ft.is_symlink() {
        0o777
    } else {
        0o644
    }
}

pub(crate) fn normalize_prefix(raw: &str) -> Result<String, Error> {
    if raw.is_empty() {
        return Ok(String::new());
    }
    normalize_rel_path(raw)
}

pub(crate) fn normalize_rel_path(raw: &str) -> Result<String, Error> {
    if raw.contains('\0') {
        return Err(Error::Source(format!("unsafe path {raw:?}")));
    }
    let mut name = raw;
    while let Some(rest) = name.strip_prefix("./") {
        name = rest;
    }
    name = name.trim_end_matches('/');
    if name.is_empty() || name == "." {
        return Ok(String::new());
    }
    if name.starts_with('/') || name.contains('\\') || name.len() >= 1024 {
        return Err(Error::Source(format!("unsafe path {raw:?}")));
    }
    for part in name.split('/') {
        if part.is_empty() || part == "." || part == ".." {
            return Err(Error::Source(format!("unsafe path {raw:?}")));
        }
    }
    Ok(name.to_owned())
}

pub(crate) fn is_apple_double(path: &str) -> bool {
    path.split('/').any(|part| part.starts_with("._"))
}

pub(crate) fn validate_ancestors(entries: &BTreeMap<String, SourceEntry>) -> Result<(), Error> {
    for path in entries.keys() {
        let mut cursor = path.as_str();
        while let Some((parent, _)) = cursor.rsplit_once('/') {
            if let Some(entry) = entries.get(parent)
                && entry.kind != SourceKind::Dir
            {
                return Err(Error::Source(format!("{path} is below non-directory {parent}")));
            }
            cursor = parent;
        }
    }
    Ok(())
}
