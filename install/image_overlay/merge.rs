// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::sync::Arc;

use switch_apfs_write::{Attrs, InodeRef, Kind, Tree};

use crate::Error;
use crate::source::{SourceKind, TreeSource, validate_ancestors};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MergeReport {
    pub dirs: usize,
    pub files: usize,
    pub replaced_files: usize,
}

/// Merges `source` onto `tree`'s root: directories `0:0 0755` (reusing existing), files `0:0 0644` (replacing existing).
pub fn merge_tree(tree: &mut Tree, source: &TreeSource) -> Result<MergeReport, Error> {
    let root = tree.root();
    merge_tree_at(tree, root, source)
}

/// Merges `source` onto directory `target` in `tree`.
pub fn merge_tree_at(tree: &mut Tree, target: InodeRef, source: &TreeSource) -> Result<MergeReport, Error> {
    if tree.inode(target).kind != Kind::Dir {
        return Err(Error::Merge("merge target is not a directory".into()));
    }
    validate_ancestors(source.entries())?;

    for (path, entry) in source.entries() {
        match &entry.kind {
            SourceKind::Symlink(_) => {
                return Err(Error::Merge(format!(
                    "{path}: symlinks are not supported in merge_tree"
                )));
            }
            SourceKind::Dir => {
                check_existing_ancestors(tree, target, path)?;
                if let Some(existing) = lookup_from(tree, target, path)
                    && tree.inode(existing).kind != Kind::Dir
                {
                    return Err(Error::Merge(format!("{path}: existing entry is not a directory")));
                }
            }
            SourceKind::File(_) => {
                check_existing_ancestors(tree, target, path)?;
                if let Some(existing) = lookup_from(tree, target, path)
                    && tree.inode(existing).kind != Kind::File
                {
                    return Err(Error::Merge(format!("{path}: existing entry is not a regular file")));
                }
            }
        }
    }

    let mut ordered: Vec<(&String, &crate::source::SourceEntry)> = source.entries().iter().collect();
    ordered.sort_by(|(a, _), (b, _)| {
        a.matches('/')
            .count()
            .cmp(&b.matches('/').count())
            .then_with(|| a.cmp(b))
    });

    let mut report = MergeReport::default();
    let mut dirs: HashMap<String, InodeRef> = HashMap::new();
    dirs.insert(String::new(), target);

    for (path, entry) in ordered {
        let (parent_path, name) = match path.rsplit_once('/') {
            Some((p, n)) => (p, n),
            None => ("", path.as_str()),
        };
        let parent = ensure_merge_dir(tree, &mut dirs, parent_path, &mut report.dirs)?;
        match &entry.kind {
            SourceKind::Dir => {
                if let Some(existing) = tree.child(parent, name) {
                    let attrs = &mut tree.inode_mut(existing).attrs;
                    attrs.uid = 0;
                    attrs.gid = 0;
                    attrs.mode = 0o755;
                    dirs.insert(path.clone(), existing);
                } else {
                    let inode = tree.add_dir(parent, name, Attrs::new(0, 0, 0o755))?;
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
                if let Some(existing) = tree.child(parent, name) {
                    tree.replace_content(existing, stream)?;
                    let attrs = &mut tree.inode_mut(existing).attrs;
                    attrs.uid = 0;
                    attrs.gid = 0;
                    attrs.mode = 0o644;
                    attrs.times = None;
                    report.replaced_files += 1;
                } else {
                    tree.add_file(parent, name, Attrs::new(0, 0, 0o644), stream)?;
                }
                report.files += 1;
            }
            SourceKind::Symlink(_) => unreachable!("symlinks rejected above"),
        }
    }

    Ok(report)
}

fn check_existing_ancestors(tree: &Tree, target: InodeRef, path: &str) -> Result<(), Error> {
    let mut current = target;
    let mut parts = path.split('/').peekable();
    while let Some(part) = parts.next() {
        if parts.peek().is_none() {
            break;
        }
        let Some(next) = tree.child(current, part) else {
            break;
        };
        if tree.inode(next).kind != Kind::Dir {
            return Err(Error::Merge(format!("{path}: ancestor {part} is not a directory")));
        }
        current = next;
    }
    Ok(())
}

fn lookup_from(tree: &Tree, target: InodeRef, path: &str) -> Option<InodeRef> {
    path.split('/').try_fold(target, |dir, name| tree.child(dir, name))
}

fn ensure_merge_dir(
    tree: &mut Tree,
    dirs: &mut HashMap<String, InodeRef>,
    rel: &str,
    created: &mut usize,
) -> Result<InodeRef, Error> {
    if let Some(&inode) = dirs.get(rel) {
        return Ok(inode);
    }
    let (parent_path, name) = match rel.rsplit_once('/') {
        Some((p, n)) => (p, n),
        None => ("", rel),
    };
    let parent = ensure_merge_dir(tree, dirs, parent_path, created)?;
    let inode = if let Some(existing) = tree.child(parent, name) {
        let attrs = &mut tree.inode_mut(existing).attrs;
        attrs.uid = 0;
        attrs.gid = 0;
        attrs.mode = 0o755;
        existing
    } else {
        *created += 1;
        tree.add_dir(parent, name, Attrs::new(0, 0, 0o755))?
    };
    dirs.insert(rel.to_owned(), inode);
    Ok(inode)
}
