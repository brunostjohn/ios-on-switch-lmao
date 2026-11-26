// SPDX-License-Identifier: GPL-3.0-or-later
//! The kernelcache's chained fixups (`LC_DYLD_CHAINED_FIXUPS`), walked as `kcinfo.fixups_all` walks them.

use super::kernelcache::Fileset;
use crate::error::{Result, refused};
use crate::macho::{self, LC_DYLD_CHAINED_FIXUPS, u16_at, u32_at, u64_at};

const DYLD_CHAINED_PTR_64_KERNEL_CACHE: u16 = 8;
const DYLD_CHAINED_PTR_START_NONE: u16 = 0xFFFF;
const DYLD_CHAINED_PTR_START_MULTI: u16 = 0x8000;
const DYLD_CHAINED_PTR_START_LAST: u16 = 0x8000;
/// `dyld_chained_starts_in_segment` up to `page_start`.
const STARTS_IN_SEGMENT: usize = 22;

/// `(address, raw)` of every chained pointer, segment by segment, page by page, along each chain.
pub(crate) fn chained_pointers(cache: &Fileset) -> Result<Vec<(u64, u64)>> {
    let data = cache.data;
    let Some(command) = cache.image.commands.iter().rfind(|c| c.cmd == LC_DYLD_CHAINED_FIXUPS) else {
        return refused("A kernelcache without chained fixups");
    };
    let (header, _) = macho::linkedit_data(data, command)?;
    let starts = header + u32_at(data, header + 4)? as usize;
    let mut pointers = Vec::new();
    for segment in 0..u32_at(data, starts)? as usize {
        let offset = u32_at(data, starts + 4 + 4 * segment)? as usize;
        if offset == 0 {
            continue;
        }
        let at = starts + offset;
        let page_size = u64::from(u16_at(data, at + 4)?);
        if u16_at(data, at + 6)? != DYLD_CHAINED_PTR_64_KERNEL_CACHE {
            return refused("Only DYLD_CHAINED_PTR_64_KERNEL_CACHE fixups are supported");
        }
        let segment_offset = u64_at(data, at + 8)?;
        for page in 0..u16_at(data, at + 20)? as usize {
            let start = u16_at(data, at + STARTS_IN_SEGMENT + 2 * page)?;
            if start == DYLD_CHAINED_PTR_START_NONE {
                continue;
            }
            let firsts = if start & DYLD_CHAINED_PTR_START_MULTI != 0 {
                overflow_starts(data, at, usize::from(start & 0x7FFF))?
            } else {
                vec![start]
            };
            for first in firsts {
                let address = u128::from(cache.base)
                    + u128::from(segment_offset)
                    + page as u128 * u128::from(page_size)
                    + u128::from(first);
                let Ok(address) = u64::try_from(address) else {
                    return refused("A chained fixup past the 64-bit address space");
                };
                chain(cache, address, &mut pointers)?;
            }
        }
    }
    Ok(pointers)
}

fn overflow_starts(data: &[u8], at: usize, mut index: usize) -> Result<Vec<u16>> {
    let mut firsts = Vec::new();
    loop {
        let start = u16_at(data, at + STARTS_IN_SEGMENT + 2 * index)?;
        firsts.push(start & 0x7FFF);
        if start & DYLD_CHAINED_PTR_START_LAST != 0 {
            return Ok(firsts);
        }
        index += 1;
    }
}

/// Follows one chain in the file from its first pointer; each pointer's `next` counts 4-byte strides.
fn chain(cache: &Fileset, mut address: u64, pointers: &mut Vec<(u64, u64)>) -> Result<()> {
    let mut at = cache.offset(address)?;
    loop {
        let raw = u64_at(cache.data, at)?;
        pointers.push((address, raw));
        let next = (raw >> 51) & 0xFFF;
        if next == 0 {
            return Ok(());
        }
        at += 4 * next as usize;
        address = match address.checked_add(4 * next) {
            Some(next_address) => next_address,
            None => return refused("A chained fixup past the 64-bit address space"),
        };
    }
}

/// The target of a `DYLD_CHAINED_PTR_64_KERNEL_CACHE` pointer, as an offset from the kernelcache's base.
pub(crate) fn target(raw: u64) -> u64 {
    raw & 0x3FFF_FFFF
}
