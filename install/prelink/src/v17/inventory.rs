// SPDX-License-Identifier: GPL-3.0-or-later
//! `emit_prelink.instruction_inventory`: the known inline data in the new components' code, and the check that every
//! PC-relative encoding in it stays inside them, since the prelink moves them and rewrites no instruction.

use sha2::{Digest, Sha256};

use crate::error::{Result, refused};
use crate::macho::Image;

/// Constant pools some assemblers put in executable sections, which must never be read as code. Each is recognised
/// only by its whole content: (prefix, size, SHA-256, offset of the prefix in the pool).
const INLINE_LITERALS: [(&str, usize, &str, usize); 9] = [
    // SHA-256's round constants.
    (
        "982f8a42",
        256,
        "74ef7306e7452d6859b6463ce496b8df30925f69e1b2969e1f3f34bbc9c6af04",
        0,
    ),
    (
        "0001000100010001",
        528,
        "f7bb30778915b574b63a17573babfdeb97251ae92fb57e7302e69783f454f92a",
        64,
    ),
    (
        "201f1e1d1c1b1a19",
        40,
        "48bdf8e57f8647f2ef3cfda5b500209c4be9b5742632db0ff3614aca8d7ee63d",
        0,
    ),
    (
        "00000000004a87c0f039fafe422e8640",
        2160,
        "1a5eb119b57d453a6123eb839b259b96f2ff4f687fc8aabfdcf965ded106df81",
        0,
    ),
    (
        "0000807f0000f042db0f493f00008039",
        1248,
        "92c7b2f87685a1e7a92cad6e7d408fdbc03bd67d1207b6579214040f413b2161",
        0,
    ),
    (
        "1f2003d500000000001018c00ee52615",
        2188,
        "c103ce1f2e204bbbfcc2069de79758b0ea399e5e7b39b2b8337cbf3d3f7f960a",
        0,
    ),
    (
        "0000c1c00000807fff799f501344d33f",
        1088,
        "22b9e038ad1cbdef1265a71f9213354287637a561eaa52f9ec25f90d10bd0a7c",
        0,
    ),
    (
        "0000c1c00000807f0000000084aec68f",
        212,
        "43ef8598a507444b21d8a2abbe4065554baaa3b6a29dbe390465cc0ba0dfbe28",
        0,
    ),
    (
        "9b1a86a049faa8bd053f4e7b9dee213e",
        56,
        "e874aeba59eebdebdf68d18b1196d1ca6aeee12b110985a856f8fe404e0c76ee",
        0,
    ),
];

/// Known inline data at its original address.
pub(crate) struct InlineData {
    pub va: u64,
    pub size: u64,
}

/// The target of an A64 PC-relative encoding at `pc`, and whether it names a 4 KiB page (ADRP).
pub(crate) fn pc_relative_target(word: u32, pc: u64) -> Option<(i128, bool)> {
    fn signed(value: u32, bits: u32) -> i128 {
        let value = i128::from(value);
        if value & (1 << (bits - 1)) != 0 {
            value - (1 << bits)
        } else {
            value
        }
    }
    let pc = i128::from(pc);
    if matches!(word & 0x9F00_0000, 0x1000_0000 | 0x9000_0000) {
        let immediate = signed(((word >> 5) & 0x7_FFFF) << 2 | ((word >> 29) & 3), 21);
        return Some(if word & 0x8000_0000 != 0 {
            ((pc & !4095) + (immediate << 12), true)
        } else {
            (pc + immediate, false)
        });
    }
    if word & 0x7C00_0000 == 0x1400_0000 {
        return Some((pc + (signed(word & 0x3FF_FFFF, 26) << 2), false));
    }
    if word & 0xFF00_0010 == 0x5400_0000 || word & 0x7E00_0000 == 0x3400_0000 || word & 0x3B00_0000 == 0x1800_0000 {
        return Some((pc + (signed((word >> 5) & 0x7_FFFF, 19) << 2), false));
    }
    if word & 0x7E00_0000 == 0x3600_0000 {
        return Some((pc + (signed((word >> 5) & 0x3FFF, 14) << 2), false));
    }
    None
}

/// The known inline data of `modules`' code, read at `code(va, filesize)`; refuses a PC-relative encoding that leaves
/// them.
pub(crate) fn inline_data<'a>(
    modules: impl Iterator<Item = &'a Image> + Clone,
    code: impl Fn(u64, u64) -> Result<&'a [u8]>,
) -> Result<Vec<InlineData>> {
    let mut exact = Intervals::default();
    let mut paged = Intervals::default();
    for segment in modules.clone().flat_map(|m| &m.segments) {
        if segment.size != 0 && segment.name != "__LINKEDIT" {
            let end = u128::from(segment.va) + u128::from(segment.size);
            exact.push(i128::from(segment.va), end as i128);
            paged.push(i128::from(segment.va & !4095), (end + 4095 & !4095) as i128);
        }
    }
    let (exact, paged) = (exact.merged(), paged.merged());
    let mut found = Vec::new();
    for segment in modules.flat_map(|m| &m.segments) {
        if segment.name != "__TEXT_EXEC" || segment.filesize == 0 {
            continue;
        }
        let blob = code(segment.va, segment.filesize)?;
        let mut literals = Intervals::default();
        for (start, size) in known_inline_literals(blob) {
            literals.push(start as i128, (start + size) as i128);
            found.push(InlineData {
                va: segment.va + start as u64,
                size: size as u64,
            });
        }
        let literals = literals.merged();
        for offset in (0..blob.len().saturating_sub(3)).step_by(4) {
            if literals.contains(offset as i128) {
                continue;
            }
            let word = u32::from_le_bytes(blob[offset..offset + 4].try_into().unwrap());
            let pc = segment.va + offset as u64;
            let Some((target, page)) = pc_relative_target(word, pc) else {
                continue;
            };
            if !(if page { &paged } else { &exact }).contains(target) {
                return refused(format!(
                    "External PC-relative encoding at {pc:#x}; no instruction rewrite supported"
                ));
            }
        }
    }
    Ok(found)
}

fn known_inline_literals(blob: &[u8]) -> Vec<(usize, usize)> {
    let mut literals = Vec::new();
    for (prefix, size, digest, prefix_offset) in INLINE_LITERALS {
        let prefix = hex::decode(prefix).unwrap();
        let mut search = 0;
        while let Some(found) = find(blob, &prefix, search) {
            if let Some(start) = found.checked_sub(prefix_offset).filter(|s| s % 4 == 0) {
                let pool = &blob[start..blob.len().min(start + size)];
                if hex::encode(Sha256::digest(pool)) == digest {
                    literals.push((start, size));
                }
            }
            search = found + 4;
        }
    }
    literals
}

fn find(haystack: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    let rest = haystack.get(from..)?;
    rest.windows(needle.len()).position(|w| w == needle).map(|at| from + at)
}

/// A union of half-open intervals.
#[derive(Default)]
struct Intervals(Vec<(i128, i128)>);

impl Intervals {
    fn push(&mut self, start: i128, end: i128) {
        if start < end {
            self.0.push((start, end));
        }
    }

    fn merged(mut self) -> Self {
        self.0.sort_unstable();
        let mut merged: Vec<(i128, i128)> = Vec::with_capacity(self.0.len());
        for (start, end) in self.0 {
            match merged.last_mut() {
                Some(last) if start <= last.1 => last.1 = last.1.max(end),
                _ => merged.push((start, end)),
            }
        }
        Intervals(merged)
    }

    fn contains(&self, x: i128) -> bool {
        let index = self.0.partition_point(|&(start, _)| start <= x);
        index > 0 && x < self.0[index - 1].1
    }
}
