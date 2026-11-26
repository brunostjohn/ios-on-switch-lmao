// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Write;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;

use super::Value;
use crate::error::Result;

/// The bytes of `ET.tostring(to_xml(value), encoding='utf-8').replace(b' />', b'/>')`, the prelinkers' writer.
/// `integer` gives the 64-bit value each integer is written as: the 17 prelink refuses one outside 64 bits.
pub(crate) fn serialize(value: &Value, integer: &dyn Fn(i128) -> Result<u64>) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    write(value, integer, &mut out)?;
    Ok(out)
}

fn write(value: &Value, integer: &dyn Fn(i128) -> Result<u64>, out: &mut Vec<u8>) -> Result<()> {
    match value {
        Value::Dict(dict) if dict.0.is_empty() => out.extend_from_slice(b"<dict/>"),
        Value::Dict(dict) => {
            out.extend_from_slice(b"<dict>");
            for (key, child) in &dict.0 {
                text_element("key", key, out);
                write(child, integer, out)?;
            }
            out.extend_from_slice(b"</dict>");
        }
        Value::Array(items) if items.is_empty() => out.extend_from_slice(b"<array/>"),
        Value::Array(items) => {
            out.extend_from_slice(b"<array>");
            for child in items {
                write(child, integer, out)?;
            }
            out.extend_from_slice(b"</array>");
        }
        Value::Bool(true) => out.extend_from_slice(b"<true/>"),
        Value::Bool(false) => out.extend_from_slice(b"<false/>"),
        Value::Integer(v) => write!(out, "<integer size=\"64\">{:#x}</integer>", integer(*v)?).unwrap(),
        Value::Data(bytes) => text_element("data", &STANDARD.encode(bytes), out),
        Value::String(text) => text_element("string", text, out),
    }
    Ok(())
}

fn text_element(tag: &str, text: &str, out: &mut Vec<u8>) {
    if text.is_empty() {
        write!(out, "<{tag}/>").unwrap();
        return;
    }
    write!(out, "<{tag}>").unwrap();
    for c in text.chars() {
        match c {
            '&' => out.extend_from_slice(b"&amp;"),
            '<' => out.extend_from_slice(b"&lt;"),
            '>' => out.extend_from_slice(b"&gt;"),
            _ => out.extend_from_slice(c.encode_utf8(&mut [0; 4]).as_bytes()),
        }
    }
    write!(out, "</{tag}>").unwrap();
}
