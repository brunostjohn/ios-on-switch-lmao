// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the selected kexts go: one coalesced range per segment kind, page-aligned in the kernelcache's frame, the
//! stub area after their text, and the slide that puts them below our kernel.

use super::kernelcache::{Closure, Fileset};
use crate::error::{Result, refused};
use crate::macho::Image;

pub(crate) const PAGE: u64 = 0x4000;
/// The stub area a64x's stubs must fit, after the kexts' text in the same segment.
pub(crate) const STUB_RESERVE: u64 = 0x10_0000;
const BL_RANGE: i128 = 1 << 27;

/// Each prelinked segment and the kext segment it coalesces, in the order the segments are appended.
pub(crate) const COALESCED: [(&str, &str); 4] = [
    ("__PRELINK_TEXT", "__TEXT"),
    ("__PLK_DATA_CONST", "__DATA_CONST"),
    ("__PLK_TEXT_EXEC", "__TEXT_EXEC"),
    ("__PRELINK_DATA", "__DATA"),
];
pub(crate) const TEXT: usize = 2;

pub(crate) struct Layout {
    /// `[lo, hi)` of each coalesced range, in Apple's addresses.
    pub ranges: [(u64, u64); 4],
    pub stub_va: u64,
    pub slide: i64,
}

impl Layout {
    pub fn new(cache: &Fileset, closure: &Closure, native: &Image, wanted_slide: Option<i64>) -> Result<Self> {
        let mut ranges = [(0, 0); 4];
        for (range, (_, source)) in ranges.iter_mut().zip(COALESCED) {
            *range = coalesced_range(cache, closure, source)?;
        }
        let (text_lo, text_hi) = ranges[TEXT];
        let stub_va = text_hi;
        let Some(next) = ranges.iter().map(|r| r.0).filter(|&lo| lo > text_lo).min() else {
            return refused("no range after the kexts' text");
        };
        if u128::from(stub_va) + u128::from(STUB_RESERVE) > u128::from(next) {
            return refused("no room for the stub area after the kexts' text");
        }
        let highest = ranges.iter().map(|r| r.1).max().unwrap_or_default();
        let Some(kbase) = native.segments.iter().filter(|s| s.size != 0).map(|s| s.va).min() else {
            return refused("our kernel has no mapped segment");
        };
        let mut limit = (i128::from(kbase) - 0x10_0000 - i128::from(highest)) & !(i128::from(PAGE) - 1);
        if limit >= 1 << 63 {
            limit -= 1 << 64;
        }
        let slide = wanted_slide.map_or(limit, i128::from);
        if slide > limit || slide.rem_euclid(i128::from(PAGE)) != 0 {
            return refused(format!(
                "slide {:#x}: the kexts must sit page-aligned at or below {:#x}",
                slide as u64, limit as u64
            ));
        }
        let Ok(slide) = i64::try_from(slide) else {
            return refused("a slide outside 64 bits");
        };
        let stub_end = i128::from(stub_va) + i128::from(STUB_RESERVE);
        for s in native.segments.iter().filter(|s| s.initprot & 4 != 0 && s.size != 0) {
            let (lo, hi) = (i128::from(s.va), i128::from(s.va) + i128::from(s.size));
            let (first, last) = (i128::from(text_lo) + i128::from(slide), stub_end + i128::from(slide));
            if (first - lo).abs() >= BL_RANGE || (last - hi).abs() >= BL_RANGE {
                return refused("kext text out of BL range of our kernel");
            }
        }
        Ok(Layout { ranges, stub_va, slide })
    }

    pub fn text_lo(&self) -> u64 {
        self.ranges[TEXT].0
    }

    /// The range holding `address` and its offset there: the first range in `COALESCED` order (`blob_at`).
    pub fn at(&self, address: u64) -> Option<(usize, usize)> {
        let index = self.ranges.iter().position(|&(lo, hi)| lo <= address && address < hi)?;
        Some((index, (address - self.ranges[index].0) as usize))
    }

    pub fn slid(&self, address: u64) -> u64 {
        address.wrapping_add_signed(self.slide)
    }

    /// Step 1: every selected kext's mapped segments, copied into its range.
    pub fn copy_segments(&self, cache: &Fileset, closure: &Closure) -> Result<[Vec<u8>; 4]> {
        let mut blobs = self.ranges.map(|(lo, hi)| vec![0; (hi - lo) as usize]);
        for id in &closure.selected {
            for s in &cache.entry(id)?.image.segments {
                if s.name == "__LINKEDIT" || s.filesize == 0 {
                    continue;
                }
                let Some((index, at)) = self.at(s.va).filter(|&(index, _)| COALESCED[index].1 == s.name) else {
                    return refused(format!("kext segment outside its coalesced range: {id} {}", s.name));
                };
                let source = cache.bytes(s.va, s.filesize)?;
                let Some(destination) = blobs[index].get_mut(at..at + source.len()) else {
                    return refused(format!("kext segment outside its coalesced range: {id} {}", s.name));
                };
                destination.copy_from_slice(source);
            }
        }
        Ok(blobs)
    }
}

fn coalesced_range(cache: &Fileset, closure: &Closure, source: &str) -> Result<(u64, u64)> {
    let mut extents = Vec::new();
    for id in &closure.selected {
        for s in &cache.entry(id)?.image.segments {
            if s.name == source && s.size != 0 {
                extents.push((s.va, u128::from(s.va) + u128::from(s.size)));
            }
        }
    }
    let (Some(lo), Some(hi)) = (extents.iter().map(|e| e.0).min(), extents.iter().map(|e| e.1).max()) else {
        return refused(format!("no prelinked kext has a {source} segment"));
    };
    let page = u128::from(PAGE);
    match u64::try_from((hi + page - 1) & !(page - 1)) {
        Ok(hi) => Ok((lo & !(PAGE - 1), hi)),
        Err(_) => refused(format!("the kexts' {source} past the 64-bit address space")),
    }
}
