// SPDX-License-Identifier: GPL-3.0-or-later
//! window_checks' /chosen rules: the zeroed slots the loader fills (boot timing, seed report), the seed carried into
//! the window, the root boot's properties, no runtime manifest inputs, and iOS 27's boot manifest hash.

use switch_loader_abi::{
    BOOT_MANIFEST_HASH_BYTES, BOOT_MANIFEST_HASH_PROPERTY, SEED_BYTES, SEED_PROPERTY, SEED_SOURCE_BYTES,
    SEED_SOURCE_PROPERTY, STAMPS_BYTES, STAMPS_PROPERTY,
};

use crate::Error;
use crate::adt::Adt;
use crate::checks::require;

const MANIFEST_PROPERTIES: &str = "/chosen/manifest-properties";
const NO_RUNTIME_MANIFEST: [&str; 5] = [
    "RAMDisk",
    "PackedRAMDisk",
    "boot-uuid",
    BOOT_MANIFEST_HASH_PROPERTY,
    "owner-boot-manifest-domain",
];

/// The ADT as the window carries it, from its offset in the image.
pub struct InWindow<'a> {
    pub image: &'a [u8],
    pub start: usize,
}

impl<'a> InWindow<'a> {
    pub fn at(&self, offset: usize, size: usize) -> &'a [u8] {
        &self.image[self.start + offset..self.start + offset + size]
    }
}

/// The ADT offsets of the slots the loader fills: 0 for a slot the ADT does not have.
pub struct Slots {
    pub stamps: usize,
    pub seed: usize,
    pub seed_source: usize,
}

/// tools/package/native_oled_adt.py root_matching.
pub fn root_matching(volume_uuid: &str) -> Vec<u8> {
    format!(
        "<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key><dict><key>UUID</key>\
         <string>{}</string></dict></dict>\0",
        volume_uuid.to_ascii_uppercase()
    )
    .into_bytes()
}

fn zero(data: &[u8]) -> bool {
    data.iter().all(|b| *b == 0)
}

fn zeroed_slot(adt: &Adt, bytes: &[u8], in_window: &InWindow, key: &str, size: u64) -> Option<Result<usize, ()>> {
    let (offset, length) = adt.property("/chosen", key).filter(|(offset, _)| *offset != 0)?;
    let fits = length as u64 == size && offset % 4 == 0 && zero(&bytes[offset..offset + length]);
    Some(if fits && zero(in_window.at(offset, length)) {
        Ok(offset)
    } else {
        Err(())
    })
}

fn slots(adt: &Adt, bytes: &[u8], in_window: &InWindow) -> Result<Slots, Error> {
    let stamps = zeroed_slot(adt, bytes, in_window, STAMPS_PROPERTY, STAMPS_BYTES).unwrap_or(Ok(0));
    require(
        stamps.is_ok(),
        "boot timing placeholder must be a zeroed, aligned, exact-size /chosen property",
    )?;
    let Some(seed_source) = zeroed_slot(adt, bytes, in_window, SEED_SOURCE_PROPERTY, SEED_SOURCE_BYTES) else {
        return Ok(Slots {
            stamps: stamps.unwrap_or(0),
            seed: 0,
            seed_source: 0,
        });
    };
    require(
        seed_source.is_ok(),
        "seed report placeholder must be a zeroed, aligned, exact-size /chosen property",
    )?;
    let (seed, seed_bytes) = adt.property("/chosen", SEED_PROPERTY).unwrap_or((0, 0));
    require(
        seed != 0
            && seed_bytes as u64 == SEED_BYTES
            && seed % 4 == 0
            && in_window.at(seed, seed_bytes) == &bytes[seed..seed + seed_bytes],
        &format!("random-seed must be a {SEED_BYTES}-byte, aligned /chosen property carried into the window"),
    )?;
    Ok(Slots {
        stamps: stamps.unwrap_or(0),
        seed,
        seed_source: seed_source.unwrap_or(0),
    })
}

fn root_boot(adt: &Adt, bytes: &[u8], in_window: &InWindow, uuid: &str) -> Result<(), Error> {
    let required: [(&str, Vec<u8>); 5] = [
        ("osenvironment", b"main\0".to_vec()),
        ("root-matching", root_matching(uuid)),
        ("crypto-hash-method", b"sha2-384\0".to_vec()),
        ("amfi-only-platform-code", 0u32.to_le_bytes().to_vec()),
        ("ephemeral-storage", 0u32.to_le_bytes().to_vec()),
    ];
    for (key, value) in required {
        let held = adt.property("/chosen", key).is_some_and(|(pos, length)| {
            length == value.len() && bytes[pos..pos + length] == *value && in_window.at(pos, length) == value
        });
        require(held, &format!("root_boot chosen property changed: {key}"))?;
    }
    Ok(())
}

fn no_runtime_manifest(adt: &Adt, manifest_hash: bool) -> Result<(), Error> {
    let only_name = adt
        .properties
        .keys()
        .all(|(path, key)| !path.starts_with(MANIFEST_PROPERTIES) || (path == MANIFEST_PROPERTIES && key == "name"));
    let manifest_inputs = adt.properties.keys().any(|(path, key)| {
        let slot = manifest_hash && path == "/chosen" && key == BOOT_MANIFEST_HASH_PROPERTY;
        !slot && NO_RUNTIME_MANIFEST.contains(&key.as_str())
    });
    require(
        adt.property(MANIFEST_PROPERTIES, "name").is_some() && only_name && !manifest_inputs,
        "root_boot requires empty runtime manifest inputs and no RAM root",
    )
}

pub fn check(
    adt: &Adt,
    bytes: &[u8],
    in_window: &InWindow,
    root_volume_uuid: &str,
    manifest_hash: Option<&[u8; BOOT_MANIFEST_HASH_BYTES as usize]>,
) -> Result<Slots, Error> {
    let slots = slots(adt, bytes, in_window)?;
    root_boot(adt, bytes, in_window, root_volume_uuid)?;
    no_runtime_manifest(adt, manifest_hash.is_some())?;
    if let Some(digest) = manifest_hash {
        let held = adt
            .property("/chosen", BOOT_MANIFEST_HASH_PROPERTY)
            .is_some_and(|(pos, length)| bytes[pos..pos + length] == *digest && in_window.at(pos, length) == digest);
        require(
            held,
            "boot-manifest-hash must be the kernel and trust caches the window carries",
        )?;
    }
    Ok(slots)
}
