// SPDX-License-Identifier: GPL-3.0-or-later
//! `Switch/pretranslated.json` (and `os.overlay.json`), byte-equal to Python's `json.dumps({"files": files},
//! indent=1) + "\n"` with `ensure_ascii=True`.

use std::fmt::Write;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ManifestFile {
    pub path: String,
    pub trust: bool,
    pub mode: String,
    pub sha256: String,
    pub cdhash: Vec<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Manifest {
    pub files: Vec<ManifestFile>,
}

impl Manifest {
    pub fn to_json(&self) -> String {
        let mut out = String::new();
        out.push_str("{\n \"files\": [");
        for (i, f) in self.files.iter().enumerate() {
            out.push_str(if i == 0 { "\n  {\n" } else { ",\n  {\n" });
            out.push_str("   \"path\": ");
            quote(&mut out, &f.path);
            out.push_str(",\n   \"trust\": ");
            out.push_str(if f.trust { "true" } else { "false" });
            out.push_str(",\n   \"mode\": ");
            quote(&mut out, &f.mode);
            out.push_str(",\n   \"sha256\": ");
            quote(&mut out, &f.sha256);
            out.push_str(",\n   \"cdhash\": [");
            for (j, c) in f.cdhash.iter().enumerate() {
                out.push_str(if j == 0 { "\n    " } else { ",\n    " });
                quote(&mut out, c);
            }
            out.push_str(if f.cdhash.is_empty() { "]\n  }" } else { "\n   ]\n  }" });
        }
        out.push_str(if self.files.is_empty() { "]\n}\n" } else { "\n ]\n}\n" });
        out
    }
}

fn quote(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            ' '..='~' => out.push(c),
            _ => {
                let mut units = [0u16; 2];
                for unit in c.encode_utf16(&mut units) {
                    write!(out, "\\u{unit:04x}").expect("writing to a String");
                }
            }
        }
    }
    out.push('"');
}
