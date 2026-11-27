// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeSet, HashMap};
use std::io::Cursor;
use std::sync::Arc;

use plist::{Dictionary, Value};
use switch_apfs_format::fs_records::SYMLINK_XATTR_NAME;
use switch_apfs_write::{Content, InodeRef, Kind, Tree, XattrValue};

use crate::Error;
use crate::source::{SourceEntry, SourceKind, TreeSource, is_apple_double};
use crate::staged_apps::{INSTALLED, STATES};

pub(crate) enum CopyEntry {
    Dir { mode: u16 },
    File { mode: u16, bytes: Arc<[u8]> },
    Symlink { mode: u16, target: String },
}

pub(crate) fn index_children(tree: &Tree) -> HashMap<InodeRef, Vec<(String, InodeRef)>> {
    let mut map: HashMap<InodeRef, Vec<(String, InodeRef)>> = HashMap::new();
    for inode in tree.inode_refs() {
        for link in &tree.inode(inode).links {
            map.entry(link.parent).or_default().push((link.name.clone(), inode));
        }
    }
    for list in map.values_mut() {
        list.sort_by(|a, b| a.0.cmp(&b.0));
    }
    map
}

pub(crate) fn read_tree_regular_bytes(tree: &Tree, inode: InodeRef, label: &str) -> Result<Arc<[u8]>, Error> {
    let node = tree.inode(inode);
    if node.kind != Kind::File {
        return Err(Error::StagedApp(format!("{label} is not a regular file")));
    }
    match node.data {
        None => Ok(Arc::from([])),
        Some(stream) => match &tree.stream(stream).content {
            Content::Bytes(bytes) => Ok(Arc::clone(bytes)),
            _ => Err(Error::StagedApp(format!("{label}: file content is not in memory"))),
        },
    }
}

pub(crate) fn parse_plist_dict(bytes: &[u8], label: &str) -> Result<Dictionary, Error> {
    let value = Value::from_reader(Cursor::new(bytes))
        .map_err(|_| Error::StagedApp(format!("invalid app metadata: {label}")))?;
    match value {
        Value::Dictionary(dict) => Ok(dict),
        _ => Err(Error::StagedApp(format!("app metadata is not a dictionary: {label}"))),
    }
}

pub(crate) fn read_tree_info_plist(tree: &Tree, app_inode: InodeRef, app_name: &str) -> Result<Dictionary, Error> {
    let Some(plist_inode) = tree.child(app_inode, "Info.plist") else {
        return Ok(Dictionary::new());
    };
    let label = format!("{app_name}/Info.plist");
    let bytes = read_tree_regular_bytes(tree, plist_inode, &label)?;
    parse_plist_dict(&bytes, &label)
}

pub(crate) fn read_optional_tree_plist(tree: &Tree, path: &str) -> Result<Dictionary, Error> {
    let Some(inode) = tree.lookup(path) else {
        return Ok(Dictionary::new());
    };
    let bytes = read_tree_regular_bytes(tree, inode, path)?;
    parse_plist_dict(&bytes, path)
}

pub(crate) fn read_source_info_plist(source: &TreeSource, path: &str, app_name: &str) -> Result<Dictionary, Error> {
    let Some(entry) = source.get(path) else {
        return Ok(Dictionary::new());
    };
    let SourceKind::File(bytes) = &entry.kind else {
        return Err(Error::StagedApp(format!("incomplete migration seed: {app_name}")));
    };
    parse_plist_dict(bytes, path)
}

pub(crate) fn read_optional_source_plist(source: &TreeSource, path: &str) -> Result<Dictionary, Error> {
    let Some(entry) = source.get(path) else {
        return Ok(Dictionary::new());
    };
    let SourceKind::File(bytes) = &entry.kind else {
        return Ok(Dictionary::new());
    };
    parse_plist_dict(bytes, path)
}

pub(crate) fn is_tree_regular_file(tree: &Tree, dir: InodeRef, rel: &str) -> bool {
    let mut current = dir;
    let mut saw_part = false;
    for part in rel.split('/') {
        if part.is_empty() || part == "." || part == ".." {
            return false;
        }
        saw_part = true;
        let Some(next) = tree.child(current, part) else {
            return false;
        };
        current = next;
    }
    saw_part && tree.inode(current).kind == Kind::File
}

pub(crate) fn read_installed(
    tree: &Tree,
    children: &HashMap<InodeRef, Vec<(String, InodeRef)>>,
) -> Result<HashMap<String, String>, Error> {
    let mut installed = HashMap::new();
    let Some(base) = tree.lookup(INSTALLED) else {
        return Ok(installed);
    };
    if tree.inode(base).kind != Kind::Dir {
        return Ok(installed);
    }
    for (_, container) in children.get(&base).into_iter().flatten() {
        if tree.inode(*container).kind != Kind::Dir {
            continue;
        }
        for (name, app) in children.get(container).into_iter().flatten() {
            if !name.ends_with(".app") || tree.inode(*app).kind != Kind::Dir {
                continue;
            }
            let Some(plist_inode) = tree.child(*app, "Info.plist") else {
                continue;
            };
            if tree.inode(plist_inode).kind != Kind::File {
                continue;
            }
            let Some(bytes) = try_read_bytes(tree, plist_inode) else {
                continue;
            };
            let info = parse_plist_dict(&bytes, &format!("{INSTALLED}/{name}/Info.plist"))?;
            let Some(exec) = info.get("CFBundleExecutable").and_then(Value::as_string) else {
                continue;
            };
            if !exec.is_empty() && is_tree_regular_file(tree, *app, exec) {
                if let (Some(id), Some(ver)) = (
                    info.get("CFBundleIdentifier").and_then(Value::as_string),
                    info.get("CFBundleVersion").and_then(Value::as_string),
                ) {
                    installed.insert(id.to_owned(), ver.to_owned());
                }
            }
        }
    }
    Ok(installed)
}

fn try_read_bytes(tree: &Tree, inode: InodeRef) -> Option<Arc<[u8]>> {
    match tree.inode(inode).data {
        None => Some(Arc::from([])),
        Some(stream) => match &tree.stream(stream).content {
            Content::Bytes(bytes) => Some(Arc::clone(bytes)),
            _ => None,
        },
    }
}

pub(crate) fn read_states(tree: &Tree) -> Result<HashMap<String, i64>, Error> {
    let mut states = HashMap::new();
    let Some(inode) = tree.lookup(STATES) else {
        return Ok(states);
    };
    let bytes = read_tree_regular_bytes(tree, inode, STATES)?;
    let dict = parse_plist_dict(&bytes, STATES)?;
    for (k, v) in dict {
        if let Some(n) = v.as_signed_integer() {
            states.insert(k, n);
        }
    }
    Ok(states)
}

pub(crate) fn collect_tree_copy(
    tree: &Tree,
    children: &HashMap<InodeRef, Vec<(String, InodeRef)>>,
    dir: InodeRef,
    prefix: &str,
    out: &mut Vec<(String, CopyEntry)>,
) -> Result<(), Error> {
    for (name, child) in children.get(&dir).into_iter().flatten() {
        let rel = if prefix.is_empty() {
            name.clone()
        } else {
            format!("{prefix}/{name}")
        };
        let node = tree.inode(*child);
        match node.kind {
            Kind::Dir => {
                out.push((rel.clone(), CopyEntry::Dir { mode: node.attrs.mode }));
                collect_tree_copy(tree, children, *child, &rel, out)?;
            }
            Kind::File => {
                let bytes = read_tree_regular_bytes(tree, *child, &rel)?;
                out.push((
                    rel,
                    CopyEntry::File {
                        mode: node.attrs.mode,
                        bytes,
                    },
                ));
            }
            Kind::Symlink => {
                let target = read_symlink_target(tree, *child, &rel)?;
                out.push((
                    rel,
                    CopyEntry::Symlink {
                        mode: node.attrs.mode,
                        target,
                    },
                ));
            }
        }
    }
    Ok(())
}

fn read_symlink_target(tree: &Tree, inode: InodeRef, label: &str) -> Result<String, Error> {
    let xattr = tree
        .inode(inode)
        .xattrs
        .iter()
        .find(|x| x.name == SYMLINK_XATTR_NAME)
        .ok_or_else(|| Error::StagedApp(format!("{label}: symlink missing target xattr")))?;
    let XattrValue::Embedded(bytes) = &xattr.value else {
        return Err(Error::StagedApp(format!("{label}: symlink target is not embedded")));
    };
    let raw = bytes.strip_suffix(&[0]).unwrap_or(bytes);
    let target =
        std::str::from_utf8(raw).map_err(|_| Error::StagedApp(format!("{label}: symlink target is not UTF-8")))?;
    Ok(target.to_owned())
}

pub(crate) fn source_seed_apps(overlay: &TreeSource, seeds_prefix: &str) -> Result<Vec<String>, Error> {
    if overlay
        .get(seeds_prefix)
        .is_some_and(|e: &SourceEntry| e.kind != SourceKind::Dir)
    {
        return Err(Error::StagedApp(format!("{seeds_prefix} is not a directory")));
    }
    let slash = format!("{seeds_prefix}/");
    let mut apps = BTreeSet::new();
    for path in overlay.entries().keys() {
        if is_apple_double(path) {
            continue;
        }
        let Some(rest) = path.strip_prefix(&slash) else {
            continue;
        };
        let top = rest.split('/').next().unwrap_or("");
        if top.ends_with(".app") {
            apps.insert(top.to_owned());
        }
    }
    Ok(apps.into_iter().collect())
}
