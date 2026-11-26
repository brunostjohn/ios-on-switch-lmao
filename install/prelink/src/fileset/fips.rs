// SPDX-License-Identifier: GPL-3.0-or-later
//! Step 4b: every kext with corecrypto's FIPS integrity check gets the MAC of its text as it now runs, read from the
//! headers while they still hold the kernelcache's addresses.

use switch_a64x::fips;

use super::Prelink;
use crate::error::{Result, refused};
use crate::macho::u32_at;

pub(crate) fn write_macs(blobs: &mut [Vec<u8>; 4], p: &Prelink) -> Result<()> {
    let mut checked = Vec::new();
    for id in &p.closure.selected {
        let entry = p.cache.entry(id)?;
        let Some((index, at)) = p.layout.at(entry.va) else {
            return refused(format!("{id}: its header is outside the coalesced segments"));
        };
        let blob = &blobs[index];
        let end = blob.len().min(at + 32 + u32_at(blob, at + 20)? as usize);
        if let Some(found) = fips::find(&blob[at..end]) {
            checked.push(found);
        }
    }
    for found in checked {
        let original = p.cache.bytes(found.text_vmaddr, found.text_size)?;
        let (Some((text, text_at)), Some((macs, mac_at))) =
            (p.layout.at(found.text_vmaddr), p.layout.at(found.mac_vmaddr))
        else {
            return refused("A FIPS text or MAC outside the coalesced segments");
        };
        let Some(Ok(mut mac)) = blobs[macs]
            .get(mac_at..mac_at + fips::MAC_LEN)
            .map(<[u8; fips::MAC_LEN]>::try_from)
        else {
            return refused("A FIPS MAC outside the coalesced segments");
        };
        let Some(translated) = blobs[text].get(text_at..text_at + original.len()) else {
            return refused("A FIPS text outside the coalesced segments");
        };
        fips::rewrite(original, translated, &mut mac);
        blobs[macs][mac_at..mac_at + fips::MAC_LEN].copy_from_slice(&mac);
    }
    Ok(())
}
