// SPDX-License-Identifier: GPL-3.0-or-later
//! What adt_edit.py finish writes into /chosen after the identity: the IPSW kind's System volume, the manifest hash
//! slot, the launchd catalogue digests, the pretranslated cache list and each role's CDHashes, in that order.

use std::collections::{BTreeMap, BTreeSet};

use switch_loader_abi::{BOOT_MANIFEST_HASH_BYTES, BOOT_MANIFEST_HASH_PROPERTY};
use switch_trustcache::{CDHASH_LEN, cdhash_list};

use crate::tree::Node;
use crate::{Error, Finish};

const UPDATES_MAGIC: &[u8; 4] = b"SWUP";
const UPDATES_VERSION: u32 = 1;
const MAX_UPDATES: usize = 64;
const MAX_PATH: usize = 1024;

fn parse_cdhash(path: &str, hex: &str) -> Result<[u8; CDHASH_LEN], Error> {
    let bytes = hex.as_bytes();
    if bytes.len() != CDHASH_LEN * 2 {
        return Err(Error::PretranslatedUpdates(format!("{path}: CDHash is not 20 bytes")));
    }
    let mut out = [0u8; CDHASH_LEN];
    for (dst, pair) in out.iter_mut().zip(bytes.as_chunks::<2>().0) {
        let nibble = |b: u8| match b {
            b'0'..=b'9' => Some(b - b'0'),
            b'a'..=b'f' => Some(b - b'a' + 10),
            b'A'..=b'F' => Some(b - b'A' + 10),
            _ => None,
        };
        let (Some(hi), Some(lo)) = (nibble(pair[0]), nibble(pair[1])) else {
            return Err(Error::PretranslatedUpdates(format!("{path}: invalid hex CDHash")));
        };
        *dst = (hi << 4) | lo;
    }
    Ok(out)
}

fn check_update_path(path: &str) -> Result<(), Error> {
    if !path.starts_with('/')
        || path.ends_with('/')
        || path.contains("//")
        || path.split('/').any(|part| part == "." || part == "..")
        || path.contains('\0')
        || path.len() >= MAX_PATH
        || path == "/.index"
    {
        return Err(Error::PretranslatedUpdates(format!(
            "{path:?} is not a valid System path"
        )));
    }
    Ok(())
}

/// tools/package/adt_edit.py with_pretranslated_updates + tools/install/overlay_index.py encode_updates.
fn encode_pretranslated_updates(raw: &[u8], pinned: &[u8]) -> Result<Vec<u8>, Error> {
    let parsed: BTreeMap<String, String> =
        serde_json::from_slice(raw).map_err(|e| Error::PretranslatedUpdates(e.to_string()))?;
    if parsed.is_empty() || parsed.len() > MAX_UPDATES {
        return Err(Error::PretranslatedUpdates(format!(
            "{} pretranslated updates: between 1 and {MAX_UPDATES}",
            parsed.len()
        )));
    }
    let trusted: BTreeSet<[u8; CDHASH_LEN]> = pinned.as_chunks::<CDHASH_LEN>().0.iter().copied().collect();
    let mut entries = Vec::with_capacity(parsed.len());
    let mut untrusted = Vec::new();
    for (path, hex) in parsed {
        check_update_path(&path)?;
        let cdhash = parse_cdhash(&path, &hex)?;
        if !trusted.contains(&cdhash) {
            untrusted.push(path);
        } else {
            entries.push((path, cdhash));
        }
    }
    if !untrusted.is_empty() {
        return Err(Error::PretranslatedUpdates(format!(
            "not in the pretranslated trust module: {}",
            untrusted.join(", ")
        )));
    }
    let mut records = Vec::with_capacity(entries.len() * (4 + CDHASH_LEN));
    let mut strings = Vec::new();
    for (path, cdhash) in &entries {
        records.extend_from_slice(&(strings.len() as u32).to_le_bytes());
        records.extend_from_slice(cdhash);
        strings.extend_from_slice(path.as_bytes());
        strings.push(0);
    }
    let mut out = Vec::with_capacity(16 + records.len() + strings.len());
    out.extend_from_slice(UPDATES_MAGIC);
    out.extend_from_slice(&UPDATES_VERSION.to_le_bytes());
    out.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    out.extend_from_slice(&(strings.len() as u32).to_le_bytes());
    out.extend_from_slice(&records);
    out.extend_from_slice(&strings);
    Ok(out)
}

/// tools/package/native_oled_adt.py root_matching.
fn root_matching(volume_uuid: &str) -> Result<Vec<u8>, Error> {
    if !volume_uuid.is_ascii() {
        return Err(Error::VolumeUuid(volume_uuid.to_owned()));
    }
    Ok(format!(
        "<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key><dict><key>UUID</key>\
         <string>{}</string></dict></dict>\0",
        volume_uuid.to_ascii_uppercase()
    )
    .into_bytes())
}

pub(crate) fn write(tree: &mut Node, kind: &Finish) -> Result<(), Error> {
    let chosen = tree.child("chosen");
    chosen.put("root-matching", root_matching(kind.root_volume_uuid)?);
    if kind.boot_manifest_hash {
        chosen.put(BOOT_MANIFEST_HASH_PROPERTY, [0; BOOT_MANIFEST_HASH_BYTES as usize]);
    }
    if let Some(digests) = kind.launchd_catalog_sha256 {
        chosen.put("switch-launchd-catalog-sha256", digests);
    }
    if let Some(list) = kind.a64x_cache_pretranslated {
        if list.is_empty() || list.len() % CDHASH_LEN != 0 {
            return Err(Error::PretranslatedList(list.len()));
        }
        chosen.put("a64x-cache-pretranslated", list);
    }
    let mut pretranslated_pinned = Vec::new();
    for (role, modules) in kind.trust_modules.roles() {
        if modules.is_empty() {
            continue;
        }
        let listed = cdhash_list(modules)?;
        if listed.is_empty() {
            return Err(Error::NoCdhashes(role));
        }
        if role == "pretranslated" {
            pretranslated_pinned = listed.clone();
        }
        chosen.put(&format!("switch-{role}-cdhashes"), listed);
    }
    if let Some(updates) = kind.pretranslated_updates {
        let encoded = encode_pretranslated_updates(updates, &pretranslated_pinned)?;
        chosen.put("switch-pretranslated-updates", encoded);
    }
    Ok(())
}
