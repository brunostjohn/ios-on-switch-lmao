// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;

use plist::{Dictionary, Value};

use crate::Error;
use crate::blob::{self, ENTITLEMENTS, ENTITLEMENTS_DER};

/// An entitlements plist as `--entitlements` takes it: the XML goes into the signature unchanged, and the
/// CoreEntitlements DER form is made from its values.
pub struct Entitlements<'a> {
    xml: &'a [u8],
    dict: Dictionary,
}

impl<'a> Entitlements<'a> {
    pub fn parse(xml: &'a [u8]) -> Result<Self, Error> {
        if !xml.starts_with(b"<?xml") {
            return Err(Error::Entitlements("entitlements must be an XML plist".into()));
        }
        match Value::from_reader(Cursor::new(xml)).map_err(|e| Error::Entitlements(e.to_string()))? {
            Value::Dictionary(dict) => Ok(Self { xml, dict }),
            _ => Err(Error::Entitlements("entitlements must be a dictionary".into())),
        }
    }

    pub fn xml_blob(&self) -> Vec<u8> {
        blob::wrap(ENTITLEMENTS, self.xml)
    }

    pub fn der_blob(&self) -> Result<Vec<u8>, Error> {
        let mut body = der_integer(1);
        body.extend(der_dict(&self.dict)?);
        Ok(blob::wrap(ENTITLEMENTS_DER, &tlv(0x70, &body)))
    }

    /// The CodeDirectory's exec-segment flags these entitlements turn on.
    pub fn exec_segment_flags(&self) -> u64 {
        const FLAGS: [(&str, u64); 7] = [
            ("get-task-allow", 0x10),
            ("run-unsigned-code", 0x10),
            ("com.apple.private.cs.debugger", 0x20),
            ("dynamic-codesigning", 0x40),
            ("com.apple.private.skip-library-validation", 0x80),
            ("com.apple.private.amfi.can-load-cdhash", 0x100),
            ("com.apple.private.amfi.can-execute-cdhash", 0x200),
        ];
        FLAGS
            .iter()
            .filter(|(key, _)| matches!(self.dict.get(key), Some(Value::Boolean(true))))
            .fold(0, |flags, (_, flag)| flags | flag)
    }
}

// CoreEntitlements orders keys by their bytes. `codesign` refuses a dictionary mixing ASCII keys with others (its
// serializer and its validator disagree on their order), so only ASCII keys are taken.
fn der_dict(dict: &Dictionary) -> Result<Vec<u8>, Error> {
    let mut entries: Vec<(&String, &Value)> = dict.iter().collect();
    entries.sort_unstable_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let mut body = Vec::new();
    for (key, value) in entries {
        if key.is_empty() || !key.is_ascii() {
            return Err(Error::Entitlements(format!(
                "entitlement key {key:?} is empty or not ASCII"
            )));
        }
        let mut entry = tlv(0x0c, key.as_bytes());
        entry.extend(der_value(value)?);
        body.extend(tlv(0x30, &entry));
    }
    Ok(tlv(0xb0, &body))
}

fn der_value(value: &Value) -> Result<Vec<u8>, Error> {
    Ok(match value {
        Value::Boolean(on) => tlv(0x01, &[if *on { 0xff } else { 0 }]),
        Value::String(text) => tlv(0x0c, text.as_bytes()),
        Value::Integer(integer) => match (integer.as_signed(), integer.as_unsigned()) {
            (Some(signed), _) => der_integer(signed),
            (None, Some(unsigned)) => der_integer(unsigned as i64),
            (None, None) => unreachable!("a plist integer is an i64 or a u64"),
        },
        Value::Array(items) => {
            let mut body = Vec::new();
            for item in items {
                body.extend(der_value(item)?);
            }
            tlv(0x30, &body)
        }
        Value::Dictionary(dict) => der_dict(dict)?,
        _ => {
            let what = "entitlements hold only booleans, integers, strings, arrays and dictionaries";
            return Err(Error::Entitlements(what.into()));
        }
    })
}

// AMFI keeps integers as 64 bits, so values from 2^63 up wrap to negative ones.
fn der_integer(value: i64) -> Vec<u8> {
    let bytes = value.to_be_bytes();
    let mut start = 0;
    while start < 7 {
        let (head, next) = (bytes[start], bytes[start + 1] & 0x80);
        if (head == 0 && next == 0) || (head == 0xff && next != 0) {
            start += 1;
        } else {
            break;
        }
    }
    tlv(0x02, &bytes[start..])
}

fn tlv(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    let length = body.len();
    if length < 0x80 {
        out.push(length as u8);
    } else {
        let bytes = (length as u64).to_be_bytes();
        let skip = bytes.iter().take_while(|b| **b == 0).count();
        out.push(0x80 | (8 - skip) as u8);
        out.extend_from_slice(&bytes[skip..]);
    }
    out.extend_from_slice(body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn integers_are_minimal_twos_complement() {
        for (value, encoded) in [
            (0, &[0x00][..]),
            (-5, &[0xfb]),
            (128, &[0x00, 0x80]),
            (-128, &[0x80]),
            (-129, &[0xff, 0x7f]),
            (255, &[0x00, 0xff]),
            (300, &[0x01, 0x2c]),
            (i64::MIN, &[0x80, 0, 0, 0, 0, 0, 0, 0]),
            (i64::MAX, &[0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]),
            (-1, &[0xff]),
        ] {
            assert_eq!(der_integer(value), tlv(0x02, encoded), "{value}");
        }
    }

    #[test]
    fn long_lengths() {
        assert_eq!(&tlv(0x30, &[0; 0x7f])[..2], &[0x30, 0x7f]);
        assert_eq!(&tlv(0x30, &[0; 0x80])[..3], &[0x30, 0x81, 0x80]);
        assert_eq!(&tlv(0x30, &[0; 0x417])[..4], &[0x30, 0x82, 0x04, 0x17]);
    }
}
