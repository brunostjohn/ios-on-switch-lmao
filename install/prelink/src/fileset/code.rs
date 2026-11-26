// SPDX-License-Identifier: GPL-3.0-or-later
//! The kexts' code (steps 2, 2b and 4): translated by a64x, the kind's sites written into the translated words,
//! branches into Apple's kernel re-aimed at ours, then the stub area appended after the kexts' text.

use switch_a64x::Kext;

use super::kernelcache::{Closure, Fileset};
use super::layout::{Layout, PAGE, STUB_RESERVE};
use super::{Prelink, Unresolved};
use crate::error::{Result, refused};
use crate::macho::{Image, Section, put_u32, u32_at};
use crate::sites::{self, Site};

/// `S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS`.
const INSTRUCTIONS: u32 = 0x8000_0400;
const BRANCH_RANGE: i128 = 1 << 27;

pub(crate) struct Translated {
    pub stubs: Vec<u8>,
    /// `[start, end)` of every data run a64x found inside instruction sections, sorted.
    islands: Vec<(u64, u128)>,
}

/// The sections a64x translates: those with instructions, of any segment.
pub(crate) fn code_sections(image: &Image) -> impl Iterator<Item = &Section> {
    image
        .segments
        .iter()
        .flat_map(|s| &s.sections)
        .filter(|s| s.flags & INSTRUCTIONS != 0 && s.size != 0)
}

/// Step 2: `arm64e kext` over the coalesced text. Every selected kext is a job, in sorted order, even one without
/// code, because the stub cursor runs on across them.
pub(crate) fn translate(text: &mut Vec<u8>, cache: &Fileset, closure: &Closure, layout: &Layout) -> Result<Translated> {
    let mut jobs = Vec::with_capacity(closure.selected.len());
    for id in &closure.selected {
        let entry = cache.entry(id)?;
        let sections: Vec<[u64; 2]> = code_sections(&entry.image).map(|s| [s.va, s.size]).collect();
        jobs.push((sections, cache.function_starts(entry)?));
    }
    let kexts: Vec<Kext> = jobs
        .iter()
        .map(|(sections, starts)| Kext {
            sections: sections.as_slice(),
            starts: starts.as_slice(),
        })
        .collect();
    let translated = match switch_a64x::translate_kexts(text, layout.text_lo(), layout.stub_va, &kexts) {
        Ok(translated) => translated,
        Err(e) => return refused(e.to_string()),
    };
    *text = translated.text;
    let mut islands: Vec<(u64, u128)> = translated
        .kexts
        .iter()
        .flat_map(|k| &k.islands)
        .map(|i| (i.va, u128::from(i.va) + 4 * u128::from(i.words)))
        .collect();
    islands.sort_unstable();
    Ok(Translated {
        stubs: translated.stubs,
        islands,
    })
}

/// Step 2b: the kind's declared sites, each original word checked in the translated text first.
pub(crate) fn write_sites(
    text: &mut Vec<u8>,
    sites: &[Site],
    cache: &Fileset,
    closure: &Closure,
    text_lo: u64,
) -> Result<()> {
    for site in sites {
        if !closure.selected.contains(&site.bundle) {
            return refused(format!(
                "{}: sites declared for a kext that is not prelinked",
                site.bundle
            ));
        }
        let image = &cache.entry(&site.bundle)?.image;
        if !code_sections(image).any(|s| s.va <= site.at && u128::from(site.at) < u128::from(s.va) + u128::from(s.size))
        {
            return refused(format!(
                "{} {:#x}: a declared site outside its code",
                site.bundle, site.at
            ));
        }
    }
    sites::apply(
        sites,
        text,
        |text, va| u32_at(text, offset(text_lo, va)?),
        |text, va, word| put_u32(text, offset(text_lo, va)?, word),
    )
}

/// Step 4: each B or BL into Apple's kernel text, outside data islands and a64x's stubs, re-aimed at the function
/// the bindings give in our kernel.
pub(crate) fn rebind_branches(
    text: &mut [u8],
    p: &Prelink,
    translated: &Translated,
    unresolved: &mut Unresolved,
) -> Result<()> {
    let (text_lo, stub_va) = (p.layout.text_lo(), i128::from(p.layout.stub_va));
    let stubs_end = stub_va + translated.stubs.len() as i128;
    let islands = &translated.islands;
    for id in &p.closure.selected {
        for section in code_sections(&p.cache.entry(id)?.image) {
            for index in 0..section.size / 4 {
                let pc = section.va + 4 * index;
                let at = offset(text_lo, pc)?;
                let word = u32_at(text, at)?;
                if word & 0x7C00_0000 != 0x1400_0000 {
                    continue;
                }
                let island = islands.partition_point(|&(start, _)| start <= pc);
                if island > 0 && u128::from(pc) < islands[island - 1].1 {
                    continue;
                }
                let target = i128::from(pc) + (sign_extend(word & 0x3FF_FFFF, 26) << 2);
                if stub_va <= target && target < stubs_end {
                    continue;
                }
                let Some(target) = u64::try_from(target).ok().filter(|&t| p.in_kernel_text(t)) else {
                    continue;
                };
                let Some(destination) = p.bindings.resolve(target).filter(|&ours| p.in_our_text(ours)) else {
                    unresolved.function(id, target);
                    continue;
                };
                let distance = i128::from(destination) - (i128::from(pc) + i128::from(p.layout.slide));
                if !(-BRANCH_RANGE..BRANCH_RANGE).contains(&distance) {
                    return refused(format!("branch out of range at {pc:x}"));
                }
                put_u32(text, at, (word & 0xFC00_0000) | ((distance >> 2) as u32 & 0x3FF_FFFF))?;
            }
        }
    }
    Ok(())
}

/// The stub area at `stub_va`, the text then padded to whole pages.
pub(crate) fn append_stubs(text: &mut Vec<u8>, layout: &Layout, stubs: &[u8]) -> Result<()> {
    if stubs.len() as u64 > STUB_RESERVE {
        return refused("stub area overflow");
    }
    let Some(gap) = ((layout.stub_va - layout.text_lo()) as usize).checked_sub(text.len()) else {
        return refused("the kexts' text runs into the stub area");
    };
    text.resize(text.len() + gap, 0);
    text.extend_from_slice(stubs);
    text.resize(text.len().next_multiple_of(PAGE as usize), 0);
    Ok(())
}

fn offset(text_lo: u64, va: u64) -> Result<usize> {
    match va.checked_sub(text_lo) {
        Some(at) => Ok(at as usize),
        None => refused(format!("{va:#x}: code below the kexts' text")),
    }
}

fn sign_extend(value: u32, bits: u32) -> i128 {
    let value = i128::from(value);
    if value & (1 << (bits - 1)) != 0 {
        value - (1 << bits)
    } else {
        value
    }
}
