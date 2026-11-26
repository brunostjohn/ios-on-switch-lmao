// SPDX-License-Identifier: GPL-3.0-or-later
//! The layout's ADT reader (native_xnu_layout.py parse_adt): every property's offset and length by node path, for
//! filling slots in place, with stricter names than the ADT's own editor and the sidecar placeholders' rules.

use std::collections::HashMap;

use crate::Error;
use crate::kernel::{MIN_WINDOW, valid_window};

const MAX_DEPTH: usize = 64;
const PROPERTY_HEADER: usize = 36;
pub const SIDECARS: [&str; 4] = ["RAMDisk", "PackedRAMDisk", "TrustCache", "NativeDataSeed"];

pub type Properties = HashMap<(String, String), (usize, usize)>;

pub struct Adt {
    pub properties: Properties,
    pub window_size: u64,
}

impl Adt {
    pub fn property(&self, path: &str, key: &str) -> Option<(usize, usize)> {
        self.properties.get(&(path.to_owned(), key.to_owned())).copied()
    }
}

fn refused(why: &str) -> Error {
    Error::Layout(why.to_owned())
}

struct Reader<'a> {
    data: &'a [u8],
    cursor: usize,
    properties: Properties,
}

impl<'a> Reader<'a> {
    fn read(&mut self, size: usize) -> Result<&'a [u8], Error> {
        if size > self.data.len() - self.cursor {
            return Err(refused("truncated ADT"));
        }
        self.cursor += size;
        Ok(&self.data[self.cursor - size..self.cursor])
    }

    fn u32(&mut self) -> Result<usize, Error> {
        Ok(u32::from_le_bytes(self.read(4)?.try_into().expect("four bytes")) as usize)
    }

    fn property_name(raw: &[u8]) -> Result<String, Error> {
        let end = raw
            .iter()
            .position(|b| *b == 0)
            .ok_or_else(|| refused("unterminated ADT property name"))?;
        let name = std::str::from_utf8(&raw[..end])
            .ok()
            .filter(|name| name.is_ascii())
            .ok_or_else(|| refused("non-ASCII ADT property name"))?;
        Ok(name.to_owned())
    }

    fn properties(&mut self, count: usize) -> Result<Vec<(String, usize, usize)>, Error> {
        let mut props: Vec<(String, usize, usize)> = Vec::with_capacity(count);
        for _ in 0..count {
            let header = self.read(PROPERTY_HEADER)?;
            let key = Self::property_name(&header[..32])?;
            let length = u32::from_le_bytes(header[32..].try_into().expect("four bytes")) as usize;
            if key.is_empty() || props.iter().any(|(k, _, _)| *k == key) {
                return Err(refused("empty or duplicate ADT property"));
            }
            let start = self.cursor;
            self.read(length)?;
            self.read(length.wrapping_neg() % 4)?;
            props.push((key, start, length));
        }
        Ok(props)
    }

    fn node_name(&self, props: &[(String, usize, usize)], depth: usize) -> Result<String, Error> {
        let (_, start, length) = props
            .iter()
            .find(|(k, _, _)| k == "name")
            .ok_or_else(|| refused("ADT node has no name"))?;
        let raw = &self.data[*start..start + length];
        if raw.last() != Some(&0) || raw[..raw.len() - 1].contains(&0) {
            return Err(refused("unterminated or embedded-NUL ADT node name"));
        }
        let name = std::str::from_utf8(&raw[..raw.len() - 1])
            .ok()
            .filter(|name| name.is_ascii())
            .ok_or_else(|| refused("non-ASCII ADT node name"))?;
        if depth > 0 && (name.is_empty() || name.contains('/')) {
            return Err(refused("invalid ADT node name"));
        }
        Ok(name.to_owned())
    }

    fn node(&mut self, depth: usize, parent: &str) -> Result<String, Error> {
        if depth > MAX_DEPTH {
            return Err(refused("ADT nesting exceeds 64 levels"));
        }
        let (count, children) = (self.u32()?, self.u32()?);
        if count > (self.data.len() - self.cursor) / PROPERTY_HEADER {
            return Err(refused("ADT property count exceeds input"));
        }
        let props = self.properties(count)?;
        let name = self.node_name(&props, depth)?;
        let path = if depth == 0 {
            "/".to_owned()
        } else {
            format!("{}/{name}", parent.trim_end_matches('/'))
        };
        for (key, start, length) in props {
            self.properties.insert((path.clone(), key), (start, length));
        }
        if children > (self.data.len() - self.cursor) / 8 {
            return Err(refused("ADT child count exceeds input"));
        }
        let mut names = Vec::with_capacity(children);
        for _ in 0..children {
            let child = self.node(depth + 1, &path)?;
            if names.contains(&child) {
                return Err(refused("duplicate ADT sibling node"));
            }
            names.push(child);
        }
        Ok(name)
    }
}

fn window_size(data: &[u8], properties: &Properties) -> Result<u64, Error> {
    let mut window = MIN_WINDOW;
    for ((path, key), (offset, length)) in properties {
        if key == "darwinboot-memory-size" {
            if path != "/chosen" || *length != 8 {
                return Err(refused("invalid ADT window property"));
            }
            window = u64::from_le_bytes(data[*offset..offset + 8].try_into().expect("eight bytes"));
            valid_window(window)?;
        }
        if SIDECARS.contains(&key.as_str()) {
            if path != "/chosen/memory-map" || *length != 16 || data[*offset..offset + 16] != [0; 16] {
                return Err(refused("sidecars require zero 16-byte /chosen/memory-map placeholders"));
            }
            if key == "PackedRAMDisk" {
                return Err(refused("PackedRAMDisk is unsupported; supply an uncompressed RAMDisk"));
            }
        }
    }
    Ok(window)
}

pub fn parse(data: &[u8]) -> Result<Adt, Error> {
    if !(8..=16 << 20).contains(&data.len()) {
        return Err(refused("ADT must be 8 bytes..16 MiB"));
    }
    let mut reader = Reader {
        data,
        cursor: 0,
        properties: HashMap::new(),
    };
    reader.node(0, "")?;
    if reader.cursor != data.len() {
        return Err(refused("trailing bytes after ADT"));
    }
    let properties = reader.properties;
    if !properties.contains_key(&("/chosen".to_owned(), "name".to_owned())) {
        return Err(refused("ADT requires /chosen"));
    }
    let window_size = window_size(data, &properties)?;
    Ok(Adt {
        properties,
        window_size,
    })
}
