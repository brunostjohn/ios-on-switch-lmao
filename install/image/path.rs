// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;

use switch_apfs_write::{Attrs, InodeRef, Kind, StreamRef, Tree};

use crate::Error;

pub fn split_rel(path: &str) -> Result<Vec<&str>, Error> {
    if path.is_empty() || path == "/" {
        return Ok(Vec::new());
    }
    let rel = path.strip_prefix('/').unwrap_or(path);
    let mut parts = Vec::new();
    for part in rel.split('/') {
        if part.is_empty() || part == "." || part == ".." || part.contains('\0') {
            return Err(Error::InvalidPath(path.into()));
        }
        parts.push(part);
    }
    Ok(parts)
}

pub fn split_parent(path: &str) -> Result<(Vec<&str>, &str), Error> {
    let mut parts = split_rel(path)?;
    let name = parts.pop().ok_or_else(|| Error::InvalidPath(path.into()))?;
    Ok((parts, name))
}

pub fn ensure_dir(tree: &mut Tree, path: &str, attrs: Attrs) -> Result<InodeRef, Error> {
    let root = tree.root();
    ensure_dir_under(tree, root, path, attrs)
}

pub fn ensure_dir_under(tree: &mut Tree, base: InodeRef, rel: &str, attrs: Attrs) -> Result<InodeRef, Error> {
    if tree.inode(base).kind != Kind::Dir {
        return Err(Error::NotDirectory(rel.into()));
    }
    let parts = split_rel(rel)?;
    ensure_parts(tree, base, &parts, &attrs)
}

fn ensure_parts(tree: &mut Tree, mut cur: InodeRef, parts: &[&str], attrs: &Attrs) -> Result<InodeRef, Error> {
    let mut built = String::new();
    for &part in parts {
        if !built.is_empty() {
            built.push('/');
        }
        built.push_str(part);
        cur = match tree.child(cur, part) {
            Some(existing) => {
                if tree.inode(existing).kind != Kind::Dir {
                    return Err(Error::NotDirectory(built));
                }
                existing
            }
            None => tree.add_dir(cur, part, attrs.clone())?,
        };
    }
    Ok(cur)
}

pub fn add_file_at(
    tree: &mut Tree,
    path: &str,
    dir_attrs: Attrs,
    file_attrs: Attrs,
    data: Option<StreamRef>,
) -> Result<InodeRef, Error> {
    let (parents, name) = split_parent(path)?;
    let root = tree.root();
    let parent = ensure_parts(tree, root, &parents, &dir_attrs)?;
    Ok(tree.add_file(parent, name, file_attrs, data)?)
}

pub fn write_file_at(
    tree: &mut Tree,
    path: &str,
    dir_attrs: Attrs,
    file_attrs: Attrs,
    data: Option<StreamRef>,
) -> Result<InodeRef, Error> {
    let (parents, name) = split_parent(path)?;
    let root = tree.root();
    let parent = ensure_parts(tree, root, &parents, &dir_attrs)?;
    if let Some(existing) = tree.child(parent, name) {
        if tree.inode(existing).kind != Kind::File {
            return Err(Error::NotRegularFile(path.into()));
        }
        tree.replace_content(existing, data)?;
        tree.inode_mut(existing).attrs = file_attrs;
        return Ok(existing);
    }
    Ok(tree.add_file(parent, name, file_attrs, data)?)
}

/// Returns `root` and every directory inode nested beneath it, in tree order.
pub fn descendant_dirs(tree: &Tree, root: InodeRef) -> Result<Vec<InodeRef>, Error> {
    if tree.inode(root).kind != Kind::Dir {
        return Err(Error::NotDirectory("descendant_dirs root".into()));
    }
    let mut inside: HashMap<InodeRef, bool> = HashMap::new();
    inside.insert(tree.root(), tree.root() == root);
    inside.insert(root, true);

    let mut out = Vec::new();
    let mut stack = Vec::new();
    for r in tree.inode_refs() {
        if tree.inode(r).kind != Kind::Dir {
            continue;
        }
        let mut cur = r;
        let result = loop {
            if let Some(&known) = inside.get(&cur) {
                break known;
            }
            stack.push(cur);
            match tree.inode(cur).links.first() {
                Some(link) => cur = link.parent,
                None => break false,
            }
        };
        for visited in stack.drain(..) {
            inside.insert(visited, result);
        }
        if result {
            out.push(r);
        }
    }
    Ok(out)
}
