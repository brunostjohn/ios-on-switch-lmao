// SPDX-License-Identifier: GPL-3.0-or-later
//! `<role>.manifest.json`, byte for byte as Python's `json.dumps(manifest, indent=1)` writes it: ASCII only, every
//! other character escaped.

use std::fmt::Write;

pub struct Manifest<'a> {
    pub role: &'a str,
    pub translator: &'a str,
    pub files: Vec<Entry>,
}

#[derive(Clone, Debug)]
pub struct Entry {
    /// The volume path, with a leading `/`.
    pub path: String,
    pub sha256_in: String,
    pub sha256_out: String,
    pub cdhash: Vec<String>,
}

impl Entry {
    pub fn file(&self) -> String {
        format!("files{}", self.path)
    }
}

impl Manifest<'_> {
    pub fn to_json(&self) -> String {
        let mut out = String::new();
        out.push_str("{\n \"role\": ");
        quote(&mut out, self.role);
        out.push_str(",\n \"translator\": ");
        quote(&mut out, self.translator);
        out.push_str(",\n \"files\": [");
        for (i, f) in self.files.iter().enumerate() {
            out.push_str(if i == 0 { "\n  {\n" } else { ",\n  {\n" });
            for (key, value) in [
                ("path", f.path.as_str()),
                ("file", f.file().as_str()),
                ("sha256_in", f.sha256_in.as_str()),
                ("sha256_out", f.sha256_out.as_str()),
            ] {
                out.push_str("   ");
                quote(&mut out, key);
                out.push_str(": ");
                quote(&mut out, value);
                out.push_str(",\n");
            }
            out.push_str("   \"cdhash\": [");
            for (j, c) in f.cdhash.iter().enumerate() {
                out.push_str(if j == 0 { "\n    " } else { ",\n    " });
                quote(&mut out, c);
            }
            out.push_str(if f.cdhash.is_empty() { "]\n  }" } else { "\n   ]\n  }" });
        }
        out.push_str(if self.files.is_empty() { "]\n}" } else { "\n ]\n}" });
        out
    }
}

/// A JSON string as Python's encoder writes it with `ensure_ascii`.
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn python_layout() {
        let m = Manifest {
            role: "os",
            translator: "0123456789abcdef",
            files: vec![Entry {
                path: "/a/\u{e9}\u{1f600}\"\t".into(),
                sha256_in: "11".into(),
                sha256_out: "22".into(),
                cdhash: vec!["aa".into(), "bb".into()],
            }],
        };
        let want = "{\n \"role\": \"os\",\n \"translator\": \"0123456789abcdef\",\n \"files\": [\n  {\n   \"path\": \
                    \"/a/\\u00e9\\ud83d\\ude00\\\"\\t\",\n   \"file\": \"files/a/\\u00e9\\ud83d\\ude00\\\"\\t\",\n   \
                    \"sha256_in\": \"11\",\n   \"sha256_out\": \"22\",\n   \"cdhash\": [\n    \"aa\",\n    \"bb\"\n   \
                    ]\n  }\n ]\n}";
        assert_eq!(m.to_json(), want);
        let empty = Manifest {
            role: "app_cryptex",
            translator: "x",
            files: vec![],
        };
        assert_eq!(
            empty.to_json(),
            "{\n \"role\": \"app_cryptex\",\n \"translator\": \"x\",\n \"files\": []\n}"
        );
    }
}
