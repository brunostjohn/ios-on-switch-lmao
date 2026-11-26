// SPDX-License-Identifier: GPL-3.0-or-later
//! Step 5: each kext's copied header made a prelinked kernelcache's (arm64 `ALL`, an empty `LC_SEGMENT_SPLIT_INFO`,
//! slid segment and section addresses, no `__LINKEDIT` mapping), and its `kmod_info.address` slid.

use std::collections::HashSet;

use super::Prelink;
use super::kernelcache::Entry;
use super::layout::Layout;
use super::pointers::KMOD_ADDRESS;
use crate::error::{Result, refused};
use crate::macho::{LC_SEGMENT_64, put_u32, put_u64, u32_at, u64_at};

const LC_SEGMENT_SPLIT_INFO: u32 = 0x1E;

pub(crate) fn rebase(blobs: &mut [Vec<u8>; 4], p: &Prelink) -> Result<()> {
    for id in &p.closure.selected {
        let entry = p.cache.entry(id)?;
        let Some((index, at)) = p.layout.at(entry.va) else {
            return refused(format!("{id}: its header is outside the coalesced segments"));
        };
        let blob = &mut blobs[index];
        put_u32(blob, at + 8, 0)?;
        mark_split(blob, at, entry, id)?;
        for command in entry.image.commands.iter().filter(|c| c.cmd == LC_SEGMENT_64) {
            rebase_segment(blob, at + (command.offset - entry.fileoff), &p.layout)?;
        }
    }
    Ok(())
}

fn rebase_segment(blob: &mut [u8], at: usize, layout: &Layout) -> Result<()> {
    let Some(name) = blob.get(at + 8..at + 24) else {
        return refused("A kext segment command outside its header");
    };
    let va = u64_at(blob, at + 24)?;
    if name == b"__LINKEDIT\0\0\0\0\0\0" {
        put_u64(blob, at + 24, layout.slid(va))?;
        put_u64(blob, at + 32, 0)?;
        return put_u64(blob, at + 48, 0);
    }
    put_u64(blob, at + 24, layout.slid(va))?;
    for section in 0..u32_at(blob, at + 64)? as usize {
        let address_at = at + 72 + 80 * section + 32;
        let address = u64_at(blob, address_at)?;
        if address != 0 {
            put_u64(blob, address_at, layout.slid(address))?;
        }
    }
    Ok(())
}

/// An empty `LC_SEGMENT_SPLIT_INFO` in the header's padding, as kcgen leaves on every kext of a prelinked kernelcache.
///
/// XNU's prelinked-kernelcache paths take it to mean the kext's segments are scattered through the cache, which ours
/// are; a fileset's kexts don't carry it. Without it kalloc_type_view_parse panics ("No support for non-split seg
/// KCs") before the console, OSKext::setVMAttributes re-protects the kext as one range and slidePrelinkedExecutable
/// frees what it takes for the kext's tail.
fn mark_split(blob: &mut [u8], at: usize, entry: &Entry, id: &str) -> Result<()> {
    let (count, size) = (u32_at(blob, at + 16)?, u32_at(blob, at + 20)?);
    let end = at + 32 + size as usize;
    let first_section = entry
        .image
        .segments
        .iter()
        .flat_map(|s| &s.sections)
        .map(|s| s.va)
        .filter(|&va| va >= entry.va)
        .min()
        .unwrap_or(entry.va);
    let padding = blob.get(end..blob.len().min(end + 16)).unwrap_or_default();
    if u128::from(entry.va) + 32 + u128::from(size) + 16 > u128::from(first_section) || padding.iter().any(|&b| b != 0)
    {
        return refused(format!("{id}: no header padding for LC_SEGMENT_SPLIT_INFO"));
    }
    for (k, word) in [LC_SEGMENT_SPLIT_INFO, 16, 0, 0].into_iter().enumerate() {
        put_u32(blob, end + 4 * k, word)?;
    }
    let (Some(count), Some(size)) = (count.checked_add(1), size.checked_add(16)) else {
        return refused(format!("{id}: no header padding for LC_SEGMENT_SPLIT_INFO"));
    };
    put_u32(blob, at + 16, count)?;
    put_u32(blob, at + 20, size)
}

/// `kmod_info.address`, which OSKext slides itself, unless step 3 wrote it as a chained pointer.
pub(crate) fn rebase_kmod_info(blobs: &mut [Vec<u8>; 4], p: &Prelink, written: &HashSet<u64>) -> Result<()> {
    for id in &p.closure.selected {
        let kmod_info = p.cache.kmod_info(id)?;
        let Some((index, at)) = p.layout.at(kmod_info) else {
            return refused(format!("{id}: kmod_info outside the coalesced segments"));
        };
        let address_at = at + KMOD_ADDRESS as usize;
        let address = u64_at(&blobs[index], address_at)?;
        let site = kmod_info.checked_add(KMOD_ADDRESS);
        if site.is_some_and(|s| written.contains(&s)) || address == 0 {
            continue;
        }
        if address >> 48 != 0xffff {
            return refused(format!("kmod_info.address of {id} is not a plain address: {address:x}"));
        }
        put_u64(&mut blobs[index], address_at, p.layout.slid(address))?;
    }
    Ok(())
}
