// SPDX-License-Identifier: GPL-3.0-or-later
//! What adt_edit.py finish writes into /chosen after the identity: the IPSW kind's System volume, the manifest hash
//! slot, the launchd catalogue digests, the pretranslated cache list and each role's CDHashes, in that order.

use crate::tree::Node;
use crate::trust::{CDHASH_BYTES, cdhash_list};
use crate::{Error, Finish};

/// tools/package/loader_abi.py BOOT_MANIFEST_HASH_PROPERTY and BOOT_MANIFEST_HASH_BYTES.
const BOOT_MANIFEST_HASH_PROPERTY: &str = "boot-manifest-hash";
const BOOT_MANIFEST_HASH_BYTES: usize = 48;

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
        chosen.put(BOOT_MANIFEST_HASH_PROPERTY, [0; BOOT_MANIFEST_HASH_BYTES]);
    }
    if let Some(digests) = kind.launchd_catalog_sha256 {
        chosen.put("switch-launchd-catalog-sha256", digests);
    }
    if let Some(list) = kind.a64x_cache_pretranslated {
        if list.is_empty() || list.len() % CDHASH_BYTES != 0 {
            return Err(Error::PretranslatedList(list.len()));
        }
        chosen.put("a64x-cache-pretranslated", list);
    }
    for (role, modules) in kind.trust_modules.roles() {
        if modules.is_empty() {
            continue;
        }
        let listed = cdhash_list(modules)?;
        if listed.is_empty() {
            return Err(Error::NoCdhashes(role));
        }
        chosen.put(&format!("switch-{role}-cdhashes"), listed);
    }
    Ok(())
}
