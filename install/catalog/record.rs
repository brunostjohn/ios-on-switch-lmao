// SPDX-License-Identifier: GPL-3.0-or-later
//! The catalog as it is published: every field is known to this installer, and anything else refuses the catalog.
//! Which steps a record runs, and on which members and assets, are names that `plan` checks per record.

use std::collections::{BTreeMap, BTreeSet};

use serde::Deserialize;

use crate::digest::{Sha256, Sha384, hex_u64};
use crate::{Error, FORMAT};

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Catalog {
    pub format: u32,
    /// The release tag the assets are published under.
    pub release: String,
    pub builds: Vec<Record>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Record {
    pub build: String,
    pub version: String,
    pub ios: u32,
    pub xnu: String,
    pub product: String,
    pub device_class: String,
    pub ipsw: Ipsw,
    /// Step name to its kind.
    pub steps: BTreeMap<String, String>,
    /// Role to its pins.
    pub members: BTreeMap<String, Member>,
    pub volumes: Volumes,
    pub derived: Derived,
    /// Role to the release asset.
    pub assets: BTreeMap<String, Asset>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Ipsw {
    pub url: String,
    pub size: u64,
}

/// A member of the IPSW's zip: where its stored bytes are, and what they become.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Member {
    pub member: String,
    pub component: String,
    /// The stored bytes, after the zip's local header.
    pub offset: u64,
    pub stored_size: u64,
    /// The zip entry's uncompressed size, which for an encrypted archive is the archive's own.
    pub size: u64,
    pub sha256: Sha256,
    /// The member's content once inflated and decrypted: `content_size` bytes.
    pub content_sha256: Sha256,
    pub deflated: bool,
    /// The build manifest's SHA-384 Digest of the zip entry.
    pub manifest_digest: Sha384,
    pub aea: Option<Aea>,
    pub container: Option<ContainerPins>,
    pub im4p: Option<Im4p>,
}

/// An Apple Encrypted Archive member's decrypted size; its symmetric key is unwrapped at install time from Apple's
/// WKMS key server using the archive's auth data.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Aea {
    pub raw_size: u64,
}

/// A disk image member's APFS container: how the content holds it (`plan::Container`), and its bytes once unwrapped.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ContainerPins {
    pub format: String,
    pub size: u64,
    pub sha256: Sha256,
}

/// An IM4P member's type and its decoded payload, for the kernelcache its arm64 slice.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Im4p {
    pub fourcc: String,
    pub payload_size: u64,
    pub payload_sha256: Sha256,
}

/// The APFS volume UUIDs of the OS image and the cryptexes, and the cryptex images' names as Preboot stages them.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Volumes {
    pub os_volume_uuid: String,
    pub system_cryptex_uuid: String,
    pub system_cryptex_image: String,
    pub app_cryptex_uuid: String,
    pub app_cryptex_image: String,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Derived {
    /// The prelinked kernel's lowest segment, which the release's loader was laid out for.
    #[serde(deserialize_with = "hex_u64")]
    pub kernel_virt_base: u64,
    pub quartzcore_metallib_sha256: Sha256,
    pub launchd_catalog_sha256: Option<Sha256>,
    pub launchd_catalog_signature_sha256: Option<Sha256>,
    /// The pretranslate run's files by name.
    #[serde(default)]
    pub pretranslated: BTreeMap<String, Sha256>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Asset {
    /// Its name in the release, and in its SHA256SUMS.
    pub name: String,
    pub size: u64,
    pub sha256: Sha256,
}

impl Catalog {
    /// The catalog, if it is the format this installer reads and holds nothing it doesn't know.
    pub fn parse(json: &[u8]) -> Result<Self, Error> {
        #[derive(Deserialize)]
        struct Envelope {
            format: u64,
        }
        let json_error = |e: serde_json::Error| Error::Json(e.to_string());
        let envelope: Envelope = serde_json::from_slice(json).map_err(json_error)?;
        if envelope.format != u64::from(FORMAT) {
            return Err(Error::Format(envelope.format));
        }
        let catalog: Self = serde_json::from_slice(json).map_err(json_error)?;
        let mut builds = BTreeSet::new();
        if let Some(twice) = catalog.builds.iter().find(|r| !builds.insert(&r.build)) {
            return Err(Error::Duplicate(twice.build.clone()));
        }
        Ok(catalog)
    }

    pub fn record(&self, build: &str) -> Option<&Record> {
        self.builds.iter().find(|record| record.build == build)
    }
}

impl Member {
    pub fn file_name(&self) -> &str {
        self.member.rsplit('/').next().unwrap_or(&self.member)
    }

    pub fn content_size(&self) -> u64 {
        self.aea.as_ref().map_or(self.size, |aea| aea.raw_size)
    }
}
