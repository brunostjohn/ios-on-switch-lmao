// SPDX-License-Identifier: GPL-3.0-or-later
//! Debian control file parsing and `Library/dpkg/status` rewriting per `tools/install/bootstrap.py`.

use std::collections::HashMap;

use crate::Error;
use crate::manifest::HELD;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ControlRecord {
    pub fields: Vec<(String, String)>,
}

impl ControlRecord {
    pub fn get(&self, key: &str) -> Option<&str> {
        self.fields.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    pub fn set(&mut self, key: &str, value: String) {
        if let Some((_, existing)) = self.fields.iter_mut().find(|(k, _)| k == key) {
            *existing = value;
        } else {
            self.fields.push((key.to_owned(), value));
        }
    }

    fn contains_key(&self, key: &str) -> bool {
        self.fields.iter().any(|(k, _)| k == key)
    }

    fn append_continuation(&mut self, key: &str, line: &str) {
        if let Some((_, value)) = self.fields.iter_mut().find(|(k, _)| k == key) {
            value.push('\n');
            value.push_str(line);
        }
    }
}

fn py_splitlines(text: &str) -> Vec<&str> {
    let mut lines = Vec::new();
    let bytes = text.as_bytes();
    let mut start = 0;
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\n' => {
                lines.push(&text[start..i]);
                i += 1;
                start = i;
            }
            b'\r' => {
                lines.push(&text[start..i]);
                i += 1;
                if i < bytes.len() && bytes[i] == b'\n' {
                    i += 1;
                }
                start = i;
            }
            _ => i += 1,
        }
    }
    if start < bytes.len() {
        lines.push(&text[start..]);
    }
    lines
}

/// Parses Debian control stanzas preserving field order, matching `tools/install/bootstrap.py:_control`.
pub fn parse_control(text: &str) -> Result<Vec<ControlRecord>, Error> {
    let mut records = Vec::new();
    let mut record = ControlRecord::default();
    let mut current_key = String::new();

    let mut lines = py_splitlines(text);
    lines.push("");

    for line in lines {
        if line.is_empty() {
            if !record.fields.is_empty() {
                records.push(record);
            }
            record = ControlRecord::default();
            current_key.clear();
        } else if line.chars().next().is_some_and(char::is_whitespace) && !current_key.is_empty() {
            record.append_continuation(&current_key, line);
        } else {
            let Some((key, value)) = line.split_once(':') else {
                return Err(Error::Dpkg("malformed dpkg status".into()));
            };
            if key.is_empty() || record.contains_key(key) {
                return Err(Error::Dpkg("malformed dpkg status".into()));
            }
            record.fields.push((key.to_owned(), value.trim_start().to_owned()));
            current_key.clear();
            current_key.push_str(key);
        }
    }

    Ok(records)
}

/// Holds core packages and upserts the `firmware` stanza, matching `tools/install/bootstrap.py:_firmware`.
pub fn rewrite_status(text: &str, version: &str) -> Result<String, Error> {
    let mut records = parse_control(text)?;

    let mut index_by_pkg: HashMap<String, usize> = HashMap::with_capacity(records.len());
    for (idx, record) in records.iter().enumerate() {
        let Some(pkg) = record.get("Package") else {
            return Err(Error::Dpkg(
                "dpkg database lacks protected core packages or lists duplicates".into(),
            ));
        };
        if pkg.is_empty() || index_by_pkg.insert(pkg.to_owned(), idx).is_some() {
            return Err(Error::Dpkg(
                "dpkg database lacks protected core packages or lists duplicates".into(),
            ));
        }
    }

    for &name in &HELD {
        let Some(&idx) = index_by_pkg.get(name) else {
            return Err(Error::Dpkg(
                "dpkg database lacks protected core packages or lists duplicates".into(),
            ));
        };
        let status = records[idx].get("Status").unwrap_or("");
        let words: Vec<&str> = status.split_whitespace().collect();
        if words.len() < 2 || words[words.len() - 2..] != ["ok", "installed"] {
            return Err(Error::Dpkg(format!("{name} is not configured")));
        }
        records[idx].set("Status", "hold ok installed".into());
    }

    let firmware_idx = match index_by_pkg.get("firmware").copied() {
        Some(idx) => idx,
        None => {
            let mut stanza = ControlRecord::default();
            stanza.set("Package", "firmware".into());
            records.push(stanza);
            records.len() - 1
        }
    };

    let firmware = &mut records[firmware_idx];
    for (key, value) in [
        ("Status", "hold ok installed"),
        ("Priority", "required"),
        ("Section", "System"),
        ("Essential", "yes"),
        ("Architecture", "iphoneos-arm64"),
        ("Version", version),
        ("Description", "iOS firmware version"),
    ] {
        firmware.set(key, value.to_owned());
    }

    Ok(serialize_control(&records))
}

fn serialize_control(records: &[ControlRecord]) -> String {
    let mut out = String::new();
    for (i, record) in records.iter().enumerate() {
        if i > 0 {
            out.push_str("\n\n");
        }
        for (j, (key, value)) in record.fields.iter().enumerate() {
            if j > 0 {
                out.push('\n');
            }
            out.push_str(key);
            out.push_str(": ");
            out.push_str(value);
        }
    }
    out.push('\n');
    out
}
