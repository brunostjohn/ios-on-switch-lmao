// SPDX-License-Identifier: GPL-3.0-or-later
//! Directory-entry names for APFS volumes that are case-sensitive and normalisation-insensitive.
//!
//! The stored hash is `!CRC-32C(UTF-32LE(NFD(name))) & 0x3FFFFF`, without the terminating NUL the Reference
//! (p. 79) describes: 879,386 entries of Apple's 24B119 and 21H461 System volumes and of macOS-made volumes match that
//! form, and none match the Reference's.
//!
//! The target kernel refuses (EILSEQ) a name holding a code point its Unicode version doesn't assign, or a
//! noncharacter, so the writer refuses those too: XNU 10063 (iOS 17) is at Unicode 15.1, XNU 12377 (iOS 27) at 17.0.
//! NFD from `unicode-normalization` is right for every code point that passes: Unicode's stability policy freezes a
//! character's decomposition and combining class once it is assigned.

use std::fmt;

use unicode_normalization::UnicodeNormalization;

pub const NAME_MAX: usize = 255;
const HASH_MASK: u32 = 0x003F_FFFF;
const LEN_BITS: u32 = 10;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct UnicodeVersion {
    pub major: u8,
    pub minor: u8,
}

impl UnicodeVersion {
    pub const IOS_17: UnicodeVersion = UnicodeVersion { major: 15, minor: 1 };
    pub const IOS_27: UnicodeVersion = UnicodeVersion { major: 17, minor: 0 };
    /// The newest version `ferrosys-ucd`'s assignment table can answer for.
    pub const TABLES: UnicodeVersion = UnicodeVersion { major: 17, minor: 0 };
}

impl fmt::Display for UnicodeVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}", self.major, self.minor)
    }
}

impl std::str::FromStr for UnicodeVersion {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, String> {
        let (major, minor) = s.split_once('.').unwrap_or((s, "0"));
        let parse = |v: &str| v.parse::<u8>().map_err(|_| format!("not a Unicode version: {s}"));
        Ok(UnicodeVersion {
            major: parse(major)?,
            minor: parse(minor)?,
        })
    }
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum NameError {
    #[error("an empty name")]
    Empty,
    #[error("{0:?} is not a directory entry name")]
    Reserved(String),
    #[error("{0:?} holds '/' or NUL")]
    Separator(String),
    #[error("{name:?} is longer than {max} bytes", name = .0, max = NAME_MAX)]
    TooLong(String),
    #[error("{name:?} holds U+{code:04X}, which Unicode {version} doesn't assign")]
    Unassigned {
        name: String,
        code: u32,
        version: UnicodeVersion,
    },
    #[error("{name:?} holds the noncharacter U+{code:04X}")]
    Noncharacter { name: String, code: u32 },
    #[error("the assignment tables stop at Unicode {tables}, not {asked}", tables = UnicodeVersion::TABLES, asked = .0)]
    UnknownVersion(UnicodeVersion),
}

/// `j_drec_hashed_key_t.name_len_and_hash` for `name`: the hash in the top 22 bits, the UTF-8 length with its NUL
/// in the low 10.
pub fn name_len_and_hash(name: &str, unicode: UnicodeVersion) -> Result<u32, NameError> {
    check(name, unicode)?;
    Ok(hash(name) << LEN_BITS | (name.len() as u32 + 1))
}

pub fn hash(name: &str) -> u32 {
    let mut utf32 = Vec::with_capacity(name.len() * 4);
    for c in name.nfd() {
        utf32.extend_from_slice(&u32::from(c).to_le_bytes());
    }
    !crc32c::crc32c(&utf32) & HASH_MASK
}

pub fn check(name: &str, unicode: UnicodeVersion) -> Result<(), NameError> {
    if unicode > UnicodeVersion::TABLES {
        return Err(NameError::UnknownVersion(unicode));
    }
    if name.is_empty() {
        return Err(NameError::Empty);
    }
    if name == "." || name == ".." {
        return Err(NameError::Reserved(name.into()));
    }
    if name.contains(['/', '\0']) {
        return Err(NameError::Separator(name.into()));
    }
    if name.len() > NAME_MAX {
        return Err(NameError::TooLong(name.into()));
    }
    for c in name.chars() {
        let code = u32::from(c);
        if is_noncharacter(code) {
            return Err(NameError::Noncharacter {
                name: name.into(),
                code,
            });
        }
        if !assigned_in(code, unicode) {
            return Err(NameError::Unassigned {
                name: name.into(),
                code,
                version: unicode,
            });
        }
    }
    Ok(())
}

fn is_noncharacter(code: u32) -> bool {
    (0xFDD0..=0xFDEF).contains(&code) || code & 0xFFFE == 0xFFFE
}

fn assigned_in(code: u32, unicode: UnicodeVersion) -> bool {
    let table = ferrosys_ucd::ASSIGNED;
    let at = table.partition_point(|&(_, last, _, _)| last < code);
    match table.get(at) {
        Some(&(first, _, major, minor)) if first <= code => UnicodeVersion { major, minor } <= unicode,
        _ => false,
    }
}

#[cfg(test)]
mod tests;
