// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::Path;

use sha2::{Digest, Sha256};

use crate::metallib::parse_functions;
use crate::{CrawlError, Result};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EmbeddedLibrary {
    pub sha256: String,
    pub bytes: Vec<u8>,
    pub functions: usize,
}

pub fn extract_embedded_header(header_path: &Path) -> Result<EmbeddedLibrary> {
    let text = fs::read_to_string(header_path)?;
    let start = text
        .find('{')
        .ok_or_else(|| CrawlError::Invalid(format!("{}: missing '{{'", header_path.display())))?;
    let end = text
        .rfind('}')
        .ok_or_else(|| CrawlError::Invalid(format!("{}: missing '}}'", header_path.display())))?;
    if end <= start {
        return Err(CrawlError::Invalid(format!("{}: empty array", header_path.display())));
    }
    let body = &text.as_bytes()[start + 1..end];
    let mut data = Vec::new();
    let mut i = 0;
    while i + 3 < body.len() {
        if body[i] == b'0' && (body[i + 1] == b'x' || body[i + 1] == b'X') {
            let b1 = body[i + 2];
            let b2 = body[i + 3];
            let boundary = i + 4 == body.len() || !body[i + 4].is_ascii_alphanumeric();
            if b1.is_ascii_hexdigit() && b2.is_ascii_hexdigit() && boundary {
                let val = (hex_digit(b1) << 4) | hex_digit(b2);
                data.push(val);
                i += 4;
                continue;
            }
        }
        i += 1;
    }
    validate_embedded(data, &header_path.display().to_string())
}

pub fn load_embedded_dir(dir: &Path) -> Result<Vec<EmbeddedLibrary>> {
    let mut paths = Vec::new();
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_file() && path.extension().and_then(|e| e.to_str()) == Some("metallib") {
            paths.push(path);
        }
    }
    paths.sort();
    let mut out = Vec::with_capacity(paths.len());
    for path in paths {
        let bytes = fs::read(&path)?;
        out.push(validate_embedded(bytes, &path.display().to_string())?);
    }
    Ok(out)
}

fn validate_embedded(bytes: Vec<u8>, source: &str) -> Result<EmbeddedLibrary> {
    let found = parse_functions(&bytes)
        .filter(|fs| !fs.is_empty())
        .ok_or_else(|| CrawlError::Invalid(format!("{source}: no Metal functions")))?;
    let sha256 = hex::encode(Sha256::digest(&bytes));
    Ok(EmbeddedLibrary {
        sha256,
        bytes,
        functions: found.len(),
    })
}

fn hex_digit(b: u8) -> u8 {
    match b {
        b'0'..=b'9' => b - b'0',
        b'a'..=b'f' => b - b'a' + 10,
        b'A'..=b'F' => b - b'A' + 10,
        _ => 0,
    }
}
