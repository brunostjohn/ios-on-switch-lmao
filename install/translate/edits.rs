// SPDX-License-Identifier: GPL-3.0-or-later
//! A checked byte-edit table (`edits_<build>.json`), read as translate_tree.py's load_edits reads it.

use std::collections::BTreeMap;

use serde_json::Value;

use crate::{Error, ffi};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Edit {
    pub fileoff: u64,
    pub stock: Vec<u8>,
    pub at: usize,
    pub patched: Vec<u8>,
}

impl Edit {
    pub(crate) fn raw(&self) -> ffi::Edit {
        ffi::Edit {
            fileoff: self.fileoff,
            stock: self.stock.as_ptr(),
            stock_len: self.stock.len(),
            at: self.at,
            patched: self.patched.as_ptr(),
            patched_len: self.patched.len(),
        }
    }
}

/// `{"files": {"/path": [{"fileoff", "stock", "at", "patched"}, ...]}}`, by volume path.
pub fn parse(json: &[u8]) -> Result<BTreeMap<String, Vec<Edit>>, Error> {
    let bad = |what: String| Error::Edits(what);
    let table: Value = serde_json::from_slice(json).map_err(|e| bad(e.to_string()))?;
    let files = match table.get("files") {
        None => return Ok(BTreeMap::new()),
        Some(Value::Object(files)) => files,
        Some(_) => return Err(bad("files must be an object".into())),
    };
    let mut out = BTreeMap::new();
    for (target, entries) in files {
        let entries = match entries {
            Value::Array(e) if target.starts_with('/') && !e.is_empty() => e,
            _ => return Err(bad(format!("invalid edit entry for {target:?}"))),
        };
        let edits = entries
            .iter()
            .map(|e| edit(e).map_err(|why| bad(format!("{target}: {why}"))))
            .collect::<Result<Vec<_>, _>>()?;
        out.insert(target.clone(), edits);
    }
    Ok(out)
}

fn edit(entry: &Value) -> Result<Edit, String> {
    let fileoff = match entry.get("fileoff") {
        Some(Value::Number(n)) => n.as_u64().ok_or_else(|| format!("invalid fileoff {n}"))?,
        Some(Value::String(s)) => python_int(s.trim()).ok_or_else(|| format!("invalid fileoff {s:?}"))?,
        other => return Err(format!("invalid fileoff {other:?}")),
    };
    let at = match entry.get("at") {
        None => 0,
        Some(Value::Number(n)) => n
            .as_u64()
            .and_then(|n| usize::try_from(n).ok())
            .ok_or_else(|| format!("invalid at {n}"))?,
        Some(other) => return Err(format!("invalid at {other}")),
    };
    let stock = match entry.get("stock") {
        Some(Value::String(s)) if !s.is_empty() => hex::decode(s).map_err(|_| "invalid hex bytes".to_string())?,
        _ => return Err("invalid hex bytes".into()),
    };
    let patched = match entry.get("patched") {
        None => Vec::new(),
        Some(Value::String(s)) => hex::decode(s).map_err(|_| "invalid hex bytes".to_string())?,
        Some(_) => return Err("invalid hex bytes".into()),
    };
    if at.checked_add(patched.len()).is_none_or(|end| end > stock.len()) {
        return Err("patch range exceeds stock length".into());
    }
    Ok(Edit {
        fileoff,
        stock,
        at,
        patched,
    })
}

/// Python's `int(s, 0)` for the forms an offset takes: decimal without leading zeros, or 0x, 0o, 0b.
fn python_int(s: &str) -> Option<u64> {
    let lower = s.to_ascii_lowercase();
    let digits = |t: &str, radix| u64::from_str_radix(&t.replace('_', ""), radix).ok();
    if let Some(t) = lower.strip_prefix("0x") {
        digits(t, 16)
    } else if let Some(t) = lower.strip_prefix("0o") {
        digits(t, 8)
    } else if let Some(t) = lower.strip_prefix("0b") {
        digits(t, 2)
    } else if s.len() > 1 && s.starts_with('0') {
        s.bytes().all(|b| b == b'0' || b == b'_').then_some(0)
    } else {
        digits(s, 10)
    }
}
