// SPDX-License-Identifier: GPL-3.0-or-later
//! The finished ADT as tools/package/native_xnu_layout.py parse_adt accepts it, which adt_edit.py finish runs before
//! it writes one: names the layout step can address, and /filesystems DoNotUseSEP.

use std::collections::HashSet;

use crate::Error;
use crate::tree::{MAX_DEPTH, Node};

const MAX_BYTES: usize = 16 << 20;
const NAME_BYTES: usize = 32;

fn node_name(node: &Node, depth: usize) -> Result<&[u8], Error> {
    let raw = node.first("name").ok_or(Error::Invalid("a node has no name"))?;
    let name = match raw.split_last() {
        Some((0, name)) if !name.contains(&0) && name.is_ascii() => name,
        _ => return Err(Error::Invalid("a node's name is not ASCII ending in its only NUL")),
    };
    if depth > 0 && (name.is_empty() || name.contains(&b'/')) {
        return Err(Error::Invalid("a node's name is empty or holds a /"));
    }
    Ok(name)
}

fn check_node(node: &Node, depth: usize) -> Result<&[u8], Error> {
    if depth > MAX_DEPTH {
        return Err(Error::Invalid("nested more than 64 levels"));
    }
    let mut names = HashSet::new();
    for property in &node.properties {
        let name = &property.name;
        if name.is_empty() || name.len() >= NAME_BYTES || !name.is_ascii() || !names.insert(name) {
            return Err(Error::Invalid(
                "a property name is empty, 32 bytes, not ASCII or repeated",
            ));
        }
        if property.placeholder {
            return Err(Error::Invalid("a property has its placeholder bit"));
        }
    }
    let name = node_name(node, depth)?;
    let mut siblings = HashSet::new();
    for child in &node.children {
        if !siblings.insert(check_node(child, depth + 1)?) {
            return Err(Error::Invalid("two sibling nodes share a name"));
        }
    }
    Ok(name)
}

pub(crate) fn check(tree: &Node, len: usize) -> Result<(), Error> {
    if !(8..=MAX_BYTES).contains(&len) {
        return Err(Error::Invalid("not 8 bytes to 16 MiB"));
    }
    check_node(tree, 0)?;
    let top = |name: &str| tree.children.iter().find(|c| c.is_named(name.as_bytes()));
    if top("chosen").is_none() {
        return Err(Error::Invalid("no /chosen"));
    }
    if top("filesystems").and_then(|f| f.first("DoNotUseSEP")).is_none() {
        return Err(Error::Invalid("no /filesystems DoNotUseSEP"));
    }
    Ok(())
}
