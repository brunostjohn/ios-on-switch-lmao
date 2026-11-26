// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{ErrorKind, Read};
use std::path::Path;

use sha2::{Digest, Sha256};

use crate::error::{Error, Result, io_at};

/// `sha256sum` output: `<hex>  <name>`, or `<hex> *<name>` for binary mode. Every non-blank line must parse, and no
/// name may repeat.
pub fn parse_sums(text: &str) -> Result<BTreeMap<String, [u8; 32]>> {
    let mut sums = BTreeMap::new();
    for (index, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let bad = || Error::Sums {
            line: index + 1,
            text: line.to_owned(),
        };
        let (hash, name) = line.split_once(' ').ok_or_else(bad)?;
        let name = name.strip_prefix([' ', '*']).ok_or_else(bad)?;
        let mut sha256 = [0u8; 32];
        hex::decode_to_slice(hash, &mut sha256).map_err(|_| bad())?;
        if name.is_empty() || sums.insert(name.to_owned(), sha256).is_some() {
            return Err(bad());
        }
    }
    Ok(sums)
}

/// Streams `path` through SHA-256 and compares it with `sha256`. For files this crate did not download itself.
pub fn verify_file(path: &Path, sha256: &[u8; 32]) -> Result<()> {
    let mut file = File::open(path).map_err(io_at(path))?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => hasher.update(&buf[..n]),
            Err(error) if error.kind() == ErrorKind::Interrupted => {}
            Err(error) => return Err(io_at(path)(error)),
        }
    }
    let got: [u8; 32] = hasher.finalize().into();
    if &got != sha256 {
        return Err(Error::HashMismatch {
            path: path.to_owned(),
            got: hex::encode(got),
            expected: hex::encode(sha256),
        });
    }
    Ok(())
}
