// SPDX-License-Identifier: GPL-3.0-or-later
//! IM4P payloads of an IPSW: the kernelcache, the device tree and the trust caches.

#[cfg(test)]
mod tests;

use image4::payload::compr_info::COMPR_ALGO_LZFSE;
use image4::{PayloadRef, Tag};
use object::macho::CPU_TYPE_ARM64;
use object::read::macho::{FatArch, MachOFatFile32};
use sha2::{Digest, Sha256, Sha384};

const LZFSE_BLOCKS: [&[u8; 4]; 4] = [b"bvx2", b"bvx1", b"bvxn", b"bvx-"];
const FAT_MAGIC: &[u8; 4] = b"\xca\xfe\xba\xbe";

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("not an IM4P: {0}")]
    Der(image4::der::Error),
    #[error("the IM4P is {got:?}, not {expected:?}")]
    Fourcc { got: String, expected: String },
    #[error("the IM4P's SHA-384 is not the build manifest's digest of it")]
    ManifestDigest,
    #[error("the payload is not LZFSE")]
    NotLzfse,
    #[error("the payload's compression algorithm {0} is not LZFSE")]
    Algorithm(u32),
    #[error("LZFSE: {0}")]
    Lzfse(lzfse_rust::Error),
    #[error("the payload decodes to {got} bytes, not the {expected} its compression info gives")]
    Size { got: usize, expected: u32 },
    #[error("fat Mach-O: {0}")]
    Fat(String),
    #[error("the kernelcache's SHA-256 is {got}, not the pinned {expected}")]
    Pin { got: String, expected: String },
}

pub type Result<T> = std::result::Result<T, Error>;

/// The payload of an IM4P of type `fourcc`, LZFSE-decoded if it is compressed, once the whole IM4P hashes to
/// `manifest_digest` (the build manifest's SHA-384 Digest of it) when one is given.
pub fn payload(im4p: &[u8], fourcc: [u8; 4], manifest_digest: Option<&[u8; 48]>) -> Result<Vec<u8>> {
    if let Some(digest) = manifest_digest
        && Sha384::digest(im4p).as_slice() != digest.as_slice()
    {
        return Err(Error::ManifestDigest);
    }
    let payload = open(im4p, fourcc)?;
    if payload.compr_info().is_some() || is_lzfse(payload.data()) {
        lzfse(&payload)
    } else {
        Ok(payload.data().to_vec())
    }
}

/// The kernelcache's LZFSE-decoded payload, reduced to its arm64 slice if it is fat, once it hashes to `pin`.
pub fn kernelcache(im4p: &[u8], pin: &[u8; 32]) -> Result<Vec<u8>> {
    let image = arm64(lzfse(&open(im4p, *b"krnl")?)?)?;
    let got = Sha256::digest(&image);
    if got.as_slice() != pin.as_slice() {
        return Err(Error::Pin {
            got: hex::encode(got),
            expected: hex::encode(pin),
        });
    }
    Ok(image)
}

/// The device tree's LZFSE-decoded payload.
pub fn device_tree(im4p: &[u8]) -> Result<Vec<u8>> {
    lzfse(&open(im4p, *b"dtre")?)
}

fn open(im4p: &[u8], fourcc: [u8; 4]) -> Result<PayloadRef<'_>> {
    let payload = PayloadRef::parse(im4p).map_err(Error::Der)?;
    if payload.tag() != Tag::from_bytes(fourcc) {
        let got = String::from_utf8_lossy(&payload.tag().to_bytes()).into_owned();
        return Err(Error::Fourcc {
            got,
            expected: String::from_utf8_lossy(&fourcc).into_owned(),
        });
    }
    Ok(payload)
}

fn is_lzfse(data: &[u8]) -> bool {
    LZFSE_BLOCKS.iter().any(|magic| data.starts_with(*magic))
}

fn lzfse(payload: &PayloadRef<'_>) -> Result<Vec<u8>> {
    let data = payload.data();
    if !is_lzfse(data) {
        return Err(Error::NotLzfse);
    }
    let expected = match payload.compr_info() {
        Some(info) if info.algo != COMPR_ALGO_LZFSE => return Err(Error::Algorithm(info.algo)),
        info => info.map(|info| info.uncompressed_size),
    };
    let mut out = Vec::with_capacity(expected.unwrap_or_default() as usize);
    lzfse_rust::decode_bytes(data, &mut out).map_err(Error::Lzfse)?;
    match expected {
        Some(expected) if out.len() != expected as usize => Err(Error::Size {
            got: out.len(),
            expected,
        }),
        _ => Ok(out),
    }
}

fn arm64(image: Vec<u8>) -> Result<Vec<u8>> {
    if !image.starts_with(FAT_MAGIC) {
        return Ok(image);
    }
    let fat = MachOFatFile32::parse(&*image).map_err(|e| Error::Fat(e.to_string()))?;
    let mut slices = fat.arches().iter().filter(|arch| arch.cputype() == CPU_TYPE_ARM64);
    let (Some(arch), None) = (slices.next(), slices.next()) else {
        return Err(Error::Fat("it does not hold exactly one arm64 slice".into()));
    };
    Ok(arch.data(&*image).map_err(|e| Error::Fat(e.to_string()))?.to_vec())
}
