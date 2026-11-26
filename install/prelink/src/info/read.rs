// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{HashMap, HashSet};

use base64::Engine;
use base64::alphabet::STANDARD;
use base64::engine::{DecodePaddingMode, GeneralPurpose, GeneralPurposeConfig};
use quick_xml::escape::resolve_xml_entity;
use quick_xml::events::{BytesRef, BytesStart, Event};
use quick_xml::{Reader, XmlVersion};

use super::{Dict, Value};
use crate::error::{Result, refused};
use crate::pyint;

/// Python's `base64.b64decode(text, validate=True)`: canonical padding, any trailing bits.
const BASE64: GeneralPurpose = GeneralPurpose::new(
    &STANDARD,
    GeneralPurposeConfig::new()
        .with_decode_allow_trailing_bits(true)
        .with_decode_padding_mode(DecodePaddingMode::RequireCanonical),
);

/// The value of the XML before the blob's first NUL.
///
/// IDs may name scalars only, as in 21H461's kernelcache: a shared container would make Python's in-place edits of one
/// entry reach the others, which a copied value can't reproduce, so a container's ID is refused.
pub(crate) fn unserialize(blob: &[u8]) -> Result<Value> {
    let xml = blob.split(|&b| b == 0).next().unwrap_or_default();
    let Ok(xml) = std::str::from_utf8(xml) else {
        return refused("Prelink info XML is not UTF-8");
    };
    let mut parser = Parser {
        reader: Reader::from_str(xml),
        refs: HashMap::new(),
    };
    parser.document()
}

struct Parser<'a> {
    reader: Reader<&'a [u8]>,
    refs: HashMap<String, Value>,
}

impl<'a> Parser<'a> {
    fn next(&mut self) -> Result<Event<'a>> {
        match self.reader.read_event() {
            Ok(event) => Ok(event),
            Err(e) => refused(format!("Prelink info XML at {}: {e}", self.reader.error_position())),
        }
    }

    fn document(&mut self) -> Result<Value> {
        let mut root = None;
        loop {
            match self.next()? {
                Event::Start(e) if root.is_none() => root = Some(self.element(&e, false)?),
                Event::Empty(e) if root.is_none() => root = Some(self.element(&e, true)?),
                Event::Text(t) if t.xml10_content().trim_ascii().is_empty() => {}
                Event::Comment(_) | Event::PI(_) => {}
                Event::Decl(_) if root.is_none() => {}
                Event::Eof => break,
                _ => return refused("Prelink info XML is not one element"),
            }
        }
        root.map_or_else(|| refused("Prelink info XML is empty"), Ok)
    }

    fn element(&mut self, start: &BytesStart<'a>, empty: bool) -> Result<Value> {
        let (id, idref) = attributes(start)?;
        if let Some(idref) = idref {
            if !empty && let Err(e) = self.reader.read_to_end(start.name()) {
                return refused(format!("Prelink info XML: {e}"));
            }
            return match self.refs.get(&idref) {
                Some(value) => Ok(value.clone()),
                None => refused(format!("Prelink info XML: IDREF {idref} names no ID before it")),
            };
        }
        let tag = start.name().as_ref().to_owned();
        let value = match tag.as_str() {
            "dict" => Value::Dict(self.dict(empty)?),
            "array" => Value::Array(self.children(empty)?),
            _ => scalar(&tag, &self.text(empty)?)?,
        };
        if let Some(id) = id {
            if matches!(value, Value::Dict(_) | Value::Array(_)) {
                return refused("Prelink info XML shares a container");
            }
            if self.refs.insert(id, value.clone()).is_some() {
                return refused("Duplicate XML object ID");
            }
        }
        Ok(value)
    }

    fn children(&mut self, empty: bool) -> Result<Vec<Value>> {
        let mut children = Vec::new();
        if empty {
            return Ok(children);
        }
        loop {
            match self.next()? {
                Event::Start(e) => children.push(self.element(&e, false)?),
                Event::Empty(e) => children.push(self.element(&e, true)?),
                Event::End(_) => return Ok(children),
                Event::GeneralRef(r) => drop(entity(&r)?),
                Event::Text(_) | Event::CData(_) | Event::Comment(_) | Event::PI(_) => {}
                _ => return refused("Malformed prelink info XML"),
            }
        }
    }

    fn dict(&mut self, empty: bool) -> Result<Dict> {
        let children = self.children(empty)?;
        if children.len() % 2 != 0 {
            return refused("Odd XML dictionary");
        }
        let mut dict = Dict(Vec::with_capacity(children.len() / 2));
        let mut seen = HashSet::new();
        let mut children = children.into_iter();
        while let (Some(key), Some(value)) = (children.next(), children.next()) {
            let Value::String(key) = key else {
                return refused("A prelink info dictionary key that is not a string");
            };
            if !seen.insert(key.clone()) {
                return refused("Duplicate XML dictionary key");
            }
            dict.0.push((key, value));
        }
        Ok(dict)
    }

    /// ElementTree's text of an element that holds no other: entities resolved, comments dropped.
    fn text(&mut self, empty: bool) -> Result<String> {
        let mut text = String::new();
        if empty {
            return Ok(text);
        }
        loop {
            match self.next()? {
                Event::Text(t) => text.push_str(&t.xml10_content()),
                Event::CData(t) => text.push_str(&t.xml10_content()),
                Event::GeneralRef(r) => text.push_str(&entity(&r)?),
                Event::Comment(_) | Event::PI(_) => {}
                Event::End(_) => return Ok(text),
                _ => return refused("A prelink info scalar that holds an element"),
            }
        }
    }
}

fn attributes(start: &BytesStart) -> Result<(Option<String>, Option<String>)> {
    let (mut id, mut idref) = (None, None);
    for attribute in start.attributes() {
        let Ok(attribute) = attribute else {
            return refused("Malformed prelink info XML attribute");
        };
        let Ok(value) = attribute.normalized_value(XmlVersion::Implicit1_0) else {
            return refused("Malformed prelink info XML attribute");
        };
        match attribute.key.as_ref() {
            "ID" => id = Some(value.into_owned()),
            "IDREF" => idref = Some(value.into_owned()),
            _ => {}
        }
    }
    Ok((id, idref))
}

fn entity(reference: &BytesRef) -> Result<String> {
    match reference.resolve_char_ref() {
        Ok(Some(c)) => Ok(c.to_string()),
        Ok(None) => match resolve_xml_entity(reference) {
            Some(text) => Ok(text.to_owned()),
            None => refused(format!("Prelink info XML: undefined entity &{};", &**reference)),
        },
        Err(_) => refused(format!(
            "Prelink info XML: invalid character reference &{};",
            &**reference
        )),
    }
}

fn scalar(tag: &str, text: &str) -> Result<Value> {
    Ok(match tag {
        "key" | "string" => Value::String(text.to_owned()),
        "integer" => match pyint::parse(if text.is_empty() { "0" } else { text }, 0) {
            Some(value) => Value::Integer(value),
            None => return refused(format!("Prelink info integer {text:?}")),
        },
        "data" => {
            let compact: String = text.chars().filter(|c| !c.is_whitespace()).collect();
            match BASE64.decode(compact) {
                Ok(bytes) => Value::Data(bytes),
                Err(_) => return refused("Prelink info data that is not base64"),
            }
        }
        "true" | "false" => Value::Bool(tag == "true"),
        _ => return refused(format!("Unsupported prelink XML element {tag}")),
    })
}
