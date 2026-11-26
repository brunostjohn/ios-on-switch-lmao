// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use crate::Error;

pub const EMBEDDED_SIGNATURE: u32 = 0xfade_0cc0;
pub const CODE_DIRECTORY: u32 = 0xfade_0c02;
pub const REQUIREMENTS: u32 = 0xfade_0c01;
pub const ENTITLEMENTS: u32 = 0xfade_7171;
pub const ENTITLEMENTS_DER: u32 = 0xfade_7172;
pub const WRAPPER: u32 = 0xfade_0b01;

pub mod slot {
    pub const CODE_DIRECTORY: u32 = 0;
    pub const INFO: u32 = 1;
    pub const REQUIREMENTS: u32 = 2;
    pub const ENTITLEMENTS: u32 = 5;
    pub const ENTITLEMENTS_DER: u32 = 7;
    pub const ALTERNATE_CODE_DIRECTORIES: std::ops::Range<u32> = 0x1000..0x1005;
    pub const SIGNATURE: u32 = 0x10000;
    pub const IDENTIFICATION: u32 = 0x10001;
}

pub fn wrap(magic: u32, payload: &[u8]) -> Vec<u8> {
    let mut blob = Vec::with_capacity(8 + payload.len());
    blob.extend_from_slice(&magic.to_be_bytes());
    blob.extend_from_slice(&(8 + payload.len() as u32).to_be_bytes());
    blob.extend_from_slice(payload);
    blob
}

/// An ad hoc signature's requirement set: no requirements at all.
pub fn empty_requirements() -> Vec<u8> {
    wrap(REQUIREMENTS, &0u32.to_be_bytes())
}

/// Blobs laid out in slot order, as `codesign` lays them out.
#[derive(Default)]
pub struct SuperBlob {
    blobs: BTreeMap<u32, Vec<u8>>,
}

impl SuperBlob {
    pub fn insert(&mut self, slot: u32, blob: Vec<u8>) {
        self.blobs.insert(slot, blob);
    }

    pub fn len(&self) -> usize {
        12 + 8 * self.blobs.len() + self.blobs.values().map(Vec::len).sum::<usize>()
    }

    pub fn build(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.len());
        out.extend_from_slice(&EMBEDDED_SIGNATURE.to_be_bytes());
        out.extend_from_slice(&(self.len() as u32).to_be_bytes());
        out.extend_from_slice(&(self.blobs.len() as u32).to_be_bytes());
        let mut offset = 12 + 8 * self.blobs.len();
        for (slot, blob) in &self.blobs {
            out.extend_from_slice(&slot.to_be_bytes());
            out.extend_from_slice(&(offset as u32).to_be_bytes());
            offset += blob.len();
        }
        for blob in self.blobs.values() {
            out.extend_from_slice(blob);
        }
        out
    }
}

/// The blobs of an existing SuperBlob, each with its 8-byte header.
pub struct Blobs<'a> {
    blobs: Vec<(u32, &'a [u8])>,
}

impl<'a> Blobs<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self, Error> {
        if data.len() < 12 || be32(data, 0) != EMBEDDED_SIGNATURE {
            return Err(Error::Signature("no embedded signature SuperBlob"));
        }
        let length = be32(data, 4) as usize;
        let count = be32(data, 8) as usize;
        if length > data.len() || 12 + 8 * count > length {
            return Err(Error::Signature("SuperBlob index past its length"));
        }
        let data = &data[..length];
        let mut blobs = Vec::with_capacity(count);
        for index in 0..count {
            let slot = be32(data, 12 + 8 * index);
            let offset = be32(data, 16 + 8 * index) as usize;
            if offset + 8 > length {
                return Err(Error::Signature("blob past the SuperBlob"));
            }
            let size = be32(data, offset + 4) as usize;
            if size < 8 || offset + size > length {
                return Err(Error::Signature("blob past the SuperBlob"));
            }
            blobs.push((slot, &data[offset..offset + size]));
        }
        Ok(Self { blobs })
    }

    pub fn get(&self, slot: u32) -> Option<&'a [u8]> {
        self.blobs.iter().find(|(s, _)| *s == slot).map(|(_, blob)| *blob)
    }

    pub fn slots(&self) -> impl Iterator<Item = u32> + '_ {
        self.blobs.iter().map(|(slot, _)| *slot)
    }
}

pub fn be32(data: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(data[at..at + 4].try_into().expect("four bytes"))
}
