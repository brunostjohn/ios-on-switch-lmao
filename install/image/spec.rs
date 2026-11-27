// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_format::volume::{
    APFS_FEATURE_HARDLINK_MAP_RECORDS, APFS_INCOMPAT_EXPANDED_RECORDS, APFS_VOL_ROLE_DATA, APFS_VOL_ROLE_PREBOOT,
};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_write::VolumeSpec;

use crate::Error;

pub const DEFAULT_VOLUME_FEATURES: u64 = APFS_FEATURE_HARDLINK_MAP_RECORDS;
/// Clone mapping (0x04) and attribution tags (0x08), matching iOS `apfs_create_fs` (`tools/install/spec.py`).
pub const DATA_ROCOMPAT_FEATURES: u64 = 0x0C;
pub const DATA_INCOMPAT_FEATURES: u64 = APFS_INCOMPAT_EXPANDED_RECORDS;
pub const DEFAULT_DATA_NAME: &str = "Switch-iOS-Data";
pub const DEFAULT_PREBOOT_NAME: &str = "Switch-iOS-Preboot";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataVolumeParams {
    pub uuid: [u8; 16],
    pub name: String,
    pub features: u64,
    pub readonly_compatible_features: u64,
    pub incompatible_features: u64,
    pub unicode: UnicodeVersion,
}

impl DataVolumeParams {
    pub fn new(uuid: [u8; 16], name: impl Into<String>, unicode: UnicodeVersion) -> Self {
        Self {
            uuid,
            name: name.into(),
            features: DEFAULT_VOLUME_FEATURES,
            readonly_compatible_features: DATA_ROCOMPAT_FEATURES,
            incompatible_features: DATA_INCOMPAT_FEATURES,
            unicode,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PrebootVolumeParams {
    pub uuid: [u8; 16],
    pub name: String,
    pub features: u64,
    pub readonly_compatible_features: u64,
    pub incompatible_features: u64,
    pub unicode: UnicodeVersion,
}

impl PrebootVolumeParams {
    pub fn new(uuid: [u8; 16], name: impl Into<String>, unicode: UnicodeVersion) -> Self {
        Self {
            uuid,
            name: name.into(),
            features: DEFAULT_VOLUME_FEATURES,
            readonly_compatible_features: 0,
            incompatible_features: 0,
            unicode,
        }
    }
}

pub fn data_volume_spec(params: &DataVolumeParams) -> VolumeSpec {
    VolumeSpec {
        uuid: params.uuid,
        name: params.name.clone(),
        role: APFS_VOL_ROLE_DATA,
        features: params.features,
        readonly_compatible_features: params.readonly_compatible_features,
        incompatible_features: params.incompatible_features,
        unicode: params.unicode,
    }
}

pub fn preboot_volume_spec(params: &PrebootVolumeParams) -> VolumeSpec {
    VolumeSpec {
        uuid: params.uuid,
        name: params.name.clone(),
        role: APFS_VOL_ROLE_PREBOOT,
        features: params.features,
        readonly_compatible_features: params.readonly_compatible_features,
        incompatible_features: params.incompatible_features,
        unicode: params.unicode,
    }
}

pub fn parse_uuid(text: &str) -> Result<[u8; 16], Error> {
    let bytes = text.as_bytes();
    if bytes.len() != 36 || bytes[8] != b'-' || bytes[13] != b'-' || bytes[18] != b'-' || bytes[23] != b'-' {
        return Err(Error::InvalidUuid(text.into()));
    }
    let mut stripped = [0u8; 32];
    let mut dst = 0;
    for &b in bytes {
        if b != b'-' {
            stripped[dst] = b;
            dst += 1;
        }
    }
    let mut out = [0u8; 16];
    hex::decode_to_slice(stripped, &mut out).map_err(|_| Error::InvalidUuid(text.into()))?;
    Ok(out)
}
