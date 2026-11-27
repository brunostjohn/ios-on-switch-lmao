// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use sha2::{Digest, Sha256};

use crate::Error;
use crate::blob::{CODE_DIRECTORY, be32};

pub const CDHASH_LEN: usize = 20;
const HASH_LEN: usize = 32;
const HASH_SHA256: u8 = 2;

pub const ADHOC: u32 = 0x2;
pub const LINKER_SIGNED: u32 = 0x2_0000;

const VERSION_SCATTER: u32 = 0x2_0100;
const VERSION_TEAM_ID: u32 = 0x2_0200;
const VERSION_CODE_LIMIT_64: u32 = 0x2_0300;
const VERSION_EXEC_SEGMENT: u32 = 0x2_0400;
const VERSION_RUNTIME: u32 = 0x2_0500;

/// `codesign` sizes the signature space as though every CodeDirectory had this version's fixed fields.
pub const ESTIMATE_VERSION: u32 = VERSION_RUNTIME;

pub fn fixed_size(version: u32) -> usize {
    match version {
        VERSION_RUNTIME.. => 0x60,
        VERSION_EXEC_SEGMENT.. => 0x58,
        VERSION_CODE_LIMIT_64.. => 0x40,
        VERSION_TEAM_ID.. => 0x34,
        _ => 0x30,
    }
}

#[derive(Clone, Copy, Default)]
pub struct ExecSegment {
    pub base: u64,
    pub limit: u64,
    pub flags: u64,
}

/// One SHA-256 CodeDirectory, with no scatter vector, team ID or pre-encryption hashes.
pub struct Builder<'a> {
    pub identifier: &'a str,
    pub flags: u32,
    pub page_shift: u8,
    pub code_limit: u32,
    pub exec_segment: ExecSegment,
    pub runtime: u32,
    pub special: BTreeMap<u32, [u8; HASH_LEN]>,
}

impl Builder<'_> {
    pub fn add_special(&mut self, slot: u32, data: &[u8]) {
        self.special.insert(slot, Sha256::digest(data).into());
    }

    pub fn version(&self) -> u32 {
        if self.runtime != 0 {
            VERSION_RUNTIME
        } else if self.exec_segment.limit > 0 {
            VERSION_EXEC_SEGMENT
        } else {
            VERSION_SCATTER
        }
    }

    fn special_slots(&self) -> u32 {
        self.special.keys().next_back().copied().unwrap_or(0)
    }

    fn code_slots(&self) -> u32 {
        match self.code_limit {
            0 => 0,
            limit => ((limit - 1) >> self.page_shift) + 1,
        }
    }

    pub fn size(&self, version: u32) -> usize {
        fixed_size(version) + self.identifier.len() + 1 + (self.special_slots() + self.code_slots()) as usize * HASH_LEN
    }

    /// The CodeDirectory over `code`, which is the file up to `code_limit`.
    pub fn build(&self, code: &[u8]) -> Vec<u8> {
        assert_eq!(
            code.len(),
            self.code_limit as usize,
            "code is the file up to its code limit"
        );
        let version = self.version();
        let ident_offset = fixed_size(version);
        let hash_offset = ident_offset + self.identifier.len() + 1 + self.special_slots() as usize * HASH_LEN;
        let length = self.size(version);
        let mut cd = Vec::with_capacity(length);
        for word in [
            CODE_DIRECTORY,
            length as u32,
            version,
            self.flags,
            hash_offset as u32,
            ident_offset as u32,
            self.special_slots(),
            self.code_slots(),
            self.code_limit,
        ] {
            cd.extend_from_slice(&word.to_be_bytes());
        }
        cd.extend_from_slice(&[HASH_LEN as u8, HASH_SHA256, 0, self.page_shift]);
        cd.extend_from_slice(&[0; 8]);
        if version >= VERSION_EXEC_SEGMENT {
            cd.extend_from_slice(&[0; 16]);
            for word in [self.exec_segment.base, self.exec_segment.limit, self.exec_segment.flags] {
                cd.extend_from_slice(&word.to_be_bytes());
            }
        }
        if version >= VERSION_RUNTIME {
            cd.extend_from_slice(&self.runtime.to_be_bytes());
            cd.extend_from_slice(&[0; 4]);
        }
        debug_assert_eq!(cd.len(), ident_offset);
        cd.extend_from_slice(self.identifier.as_bytes());
        cd.push(0);
        for slot in (1..=self.special_slots()).rev() {
            cd.extend_from_slice(self.special.get(&slot).unwrap_or(&[0; HASH_LEN]));
        }
        for page in code.chunks(1 << self.page_shift) {
            cd.extend_from_slice(&Sha256::digest(page));
        }
        debug_assert_eq!(cd.len(), length);
        cd
    }
}

pub fn cdhash(cd: &[u8]) -> [u8; CDHASH_LEN] {
    Sha256::digest(cd)[..CDHASH_LEN]
        .try_into()
        .expect("a SHA-256 is longer than a CDHash")
}

/// What `--preserve-metadata=identifier,flags,runtime` takes from an existing CodeDirectory.
pub struct Existing {
    pub identifier: String,
    pub flags: u32,
    pub runtime: u32,
}

impl Existing {
    pub fn parse(cd: &[u8]) -> Result<Self, Error> {
        if cd.len() < 0x2c || be32(cd, 0) != CODE_DIRECTORY {
            return Err(Error::Signature("slot 0 is not a CodeDirectory"));
        }
        let version = be32(cd, 8);
        if cd[37] != HASH_SHA256 {
            return Err(Error::Unsupported("an existing CodeDirectory that is not SHA-256"));
        }
        let ident = be32(cd, 20) as usize;
        let end = cd
            .get(ident..)
            .and_then(|rest| rest.iter().position(|b| *b == 0))
            .ok_or(Error::Signature("CodeDirectory identifier past its end"))?;
        let identifier = std::str::from_utf8(&cd[ident..ident + end])
            .map_err(|_| Error::Signature("CodeDirectory identifier is not UTF-8"))?
            .to_owned();
        let runtime = if version >= VERSION_RUNTIME && cd.len() >= 0x60 {
            be32(cd, 0x58)
        } else {
            0
        };
        Ok(Self {
            identifier,
            flags: be32(cd, 12),
            runtime,
        })
    }
}
