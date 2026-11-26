// SPDX-License-Identifier: GPL-3.0-or-later
//! The part of a build's catalog record the boot files are made from: everything a step takes that is data rather
//! than a file, all of it from `ipsw/pins.bzl`. Adding a build means adding its record; no step names a build.

use serde::Deserialize;

use crate::Error;

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Record {
    pub build: String,
    /// The prelinked kernel's lowest segment, `0x…` (derived `kernel_virt_base`).
    pub kernel_virt_base: String,
    /// The System volume's UUID, which `/chosen/root-matching` names (volumes `os_volume_uuid`).
    pub os_volume_uuid: String,
    /// `/chosen/boot-manifest-hash` holds the SHA-384 of the kernel and the trust bundle.
    pub boot_manifest_hash: bool,
    pub prelink: Prelink,
    /// The kernelcache's decompressed arm64 payload (derived `kernelcache_sha256`).
    pub kernelcache_sha256: String,
    /// The build manifest's SHA-384 digest of each IM4P member.
    pub manifest_digests: ManifestDigests,
    /// Apple's launchd catalogue and its signature, for a build whose boot carries the catalogue with our jobs.
    pub launchd_catalog: Option<LaunchdPins>,
}

/// How Apple's kexts get into the kernel: from a prelinked kernelcache (iOS 17), or from a fileset one through CI's
/// bindings export (iOS 26 on).
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Prelink {
    Native,
    Fileset,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ManifestDigests {
    pub device_tree: String,
    pub static_trust_cache: String,
    pub system_cryptex_trust_cache: String,
    pub app_cryptex_trust_cache: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LaunchdPins {
    pub sha256: String,
    pub signature_sha256: String,
}

impl Record {
    pub fn from_json(json: &[u8]) -> Result<Self, Error> {
        serde_json::from_slice(json).map_err(|e| Error::Record(e.to_string()))
    }

    pub(crate) fn virt_base(&self) -> Result<u64, Error> {
        self.kernel_virt_base
            .strip_prefix("0x")
            .and_then(|hex| u64::from_str_radix(hex, 16).ok())
            .ok_or_else(|| Error::Record(format!("kernel_virt_base {:?} is not 0x-hex", self.kernel_virt_base)))
    }
}

pub(crate) fn digest<const N: usize>(what: &str, hex: &str) -> Result<[u8; N], Error> {
    let mut out = [0; N];
    hex::decode_to_slice(hex, &mut out).map_err(|_| Error::Record(format!("{what} is not {N} bytes of hex")))?;
    Ok(out)
}
