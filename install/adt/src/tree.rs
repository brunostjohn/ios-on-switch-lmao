// SPDX-License-Identifier: GPL-3.0-or-later
//! Apple device trees as tools/package/adt_edit.py reads and writes them. Written back, a property keeps only its name
//! up to the first NUL and the top bit of its size word, and its padding is zeros.

use crate::{Error, Input};

const NAME_BYTES: usize = 32;
const PLACEHOLDER: u32 = 0x8000_0000;
pub(crate) const MAX_DEPTH: usize = 64;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Property {
    pub name: Vec<u8>,
    pub placeholder: bool,
    pub value: Vec<u8>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Node {
    pub properties: Vec<Property>,
    pub children: Vec<Node>,
}

fn padding(len: usize) -> usize {
    (4 - len % 4) % 4
}

pub(crate) fn until_nul(value: &[u8]) -> &[u8] {
    value.split(|&b| b == 0).next().unwrap_or(value)
}

struct Reader<'a> {
    data: &'a [u8],
    at: usize,
    input: Input,
}

impl<'a> Reader<'a> {
    fn take(&mut self, len: usize) -> Result<&'a [u8], Error> {
        let end = self
            .at
            .checked_add(len)
            .filter(|&end| end <= self.data.len())
            .ok_or(Error::Truncated(self.input, self.at))?;
        let bytes = &self.data[self.at..end];
        self.at = end;
        Ok(bytes)
    }

    fn u32(&mut self) -> Result<u32, Error> {
        let bytes = self.take(4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    fn node(&mut self, depth: usize) -> Result<Node, Error> {
        if depth > MAX_DEPTH {
            return Err(Error::TooDeep(self.input));
        }
        let properties = self.u32()?;
        let children = self.u32()?;
        let mut node = Node::default();
        for _ in 0..properties {
            let name = until_nul(self.take(NAME_BYTES)?).to_vec();
            let word = self.u32()?;
            let len = (word & !PLACEHOLDER) as usize;
            let value = self.take(len)?.to_vec();
            self.take(padding(len))?;
            node.properties.push(Property {
                name,
                placeholder: word & PLACEHOLDER != 0,
                value,
            });
        }
        for _ in 0..children {
            node.children.push(self.node(depth + 1)?);
        }
        Ok(node)
    }
}

impl Node {
    pub(crate) fn parse(data: &[u8], input: Input) -> Result<Node, Error> {
        let mut reader = Reader { data, at: 0, input };
        let node = reader.node(0)?;
        if reader.at != data.len() {
            return Err(Error::Trailing(input, data.len() - reader.at));
        }
        Ok(node)
    }

    pub(crate) fn serialize(&self) -> Vec<u8> {
        let mut out = Vec::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&(self.properties.len() as u32).to_le_bytes());
        out.extend_from_slice(&(self.children.len() as u32).to_le_bytes());
        for property in &self.properties {
            let mut name = [0; NAME_BYTES];
            let len = property.name.len().min(NAME_BYTES);
            name[..len].copy_from_slice(&property.name[..len]);
            out.extend_from_slice(&name);
            let flag = if property.placeholder { PLACEHOLDER } else { 0 };
            out.extend_from_slice(&(flag | property.value.len() as u32).to_le_bytes());
            out.extend_from_slice(&property.value);
            out.resize(out.len() + padding(property.value.len()), 0);
        }
        for child in &self.children {
            child.write(out);
        }
    }

    pub(crate) fn named(name: &str) -> Node {
        let mut node = Node::default();
        node.put("name", [name.as_bytes(), b"\0"].concat());
        node
    }

    pub(crate) fn is_named(&self, name: &[u8]) -> bool {
        self.properties
            .iter()
            .any(|p| p.name == b"name" && until_nul(&p.value) == name)
    }

    /// The value of the first property called `name`.
    pub(crate) fn first(&self, name: &str) -> Option<&[u8]> {
        self.properties
            .iter()
            .find(|p| p.name == name.as_bytes())
            .map(|p| p.value.as_slice())
    }

    /// The value of the last property called `name`, as adt_edit.py's dict of Apple's properties has it.
    pub(crate) fn last(&self, name: &str) -> Option<&[u8]> {
        self.properties
            .iter()
            .rev()
            .find(|p| p.name == name.as_bytes())
            .map(|p| p.value.as_slice())
    }

    /// Replaces the first property called `name` where it stands, keeping its placeholder bit, or appends one.
    pub(crate) fn put(&mut self, name: &str, value: impl Into<Vec<u8>>) {
        let value = value.into();
        match self.properties.iter_mut().find(|p| p.name == name.as_bytes()) {
            Some(property) => property.value = value,
            None => self.properties.push(Property {
                name: name.as_bytes().to_vec(),
                placeholder: false,
                value,
            }),
        }
    }

    /// The first child called `name`, appended when there is none.
    pub(crate) fn child(&mut self, name: &str) -> &mut Node {
        let at = match self.children.iter().position(|c| c.is_named(name.as_bytes())) {
            Some(at) => at,
            None => {
                self.children.push(Node::named(name));
                self.children.len() - 1
            }
        };
        &mut self.children[at]
    }
}
