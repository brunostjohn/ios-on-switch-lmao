// SPDX-License-Identifier: GPL-3.0-or-later
//! `emit_prelink.instruction_inventory`: the known inline data in the new components' code, and the check that every
//! PC-relative encoding in it stays inside them, since the prelink moves them and rewrites no instruction.

use sha2::{Digest, Sha256};

use crate::error::{Result, refused};
use crate::macho::Image;

/// Algorithm tables some assemblers put in executable sections, which must never be read as code, in any build: the
/// SHA-256 round constants, the vector-repeat pool and the adler32 vector. Each is recognised only by its whole
/// content: (prefix, size, SHA-256, offset of the prefix in the pool).
const ALGORITHM_POOLS: [(&str, usize, &str, usize); 3] = [
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
];
/// A build's own pools are matched by their first bytes, as `emit_prelink.MATH_LITERAL_POOLS` lists them.
const BUILD_POOL_PREFIX: usize = 16;

/// An inline-data pool, recognised by its whole content.
pub(crate) struct Pool {
    prefix: Vec<u8>,
    size: usize,
    sha256: String,
    prefix_offset: usize,
}

/// The pools to look for: the algorithm tables, then the build's own pools, which the inherited report found in
/// Apple's code (21H461's Libm), each read back at its address with `code(va, size)` and checked against its hash.
pub(crate) fn pools<'k, 'a>(
    known: impl Iterator<Item = (u64, u64, &'k str)>,
    code: impl Fn(u64, u64) -> Result<&'a [u8]>,
) -> Result<Vec<Pool>> {
    let mut pools: Vec<Pool> = ALGORITHM_POOLS
        .iter()
        .map(|&(prefix, size, sha256, prefix_offset)| Pool {
            prefix: hex::decode(prefix).unwrap(),
            size,
            sha256: sha256.to_owned(),
            prefix_offset,
        })
        .collect();
    for (va, size, sha256) in known {
        if pools.iter().any(|p| p.sha256 == sha256) {
            continue;
        }
        let bytes = code(va, size)?;
        if hex::encode(Sha256::digest(bytes)) != sha256 {
            return refused(format!(
                "Inherited inline data at {va:#x} differs from the kernelcache's"
            ));
        }
        pools.push(Pool {
            prefix: bytes[..bytes.len().min(BUILD_POOL_PREFIX)].to_vec(),
            size: bytes.len(),
            sha256: sha256.to_owned(),
            prefix_offset: 0,
        });
    }
    Ok(pools)
}

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

/// The `pools` in `modules`' code, read at `code(va, filesize)`; refuses a PC-relative encoding that leaves them.
pub(crate) fn inline_data<'a>(
    modules: impl Iterator<Item = &'a Image> + Clone,
    pools: &[Pool],
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
        for (start, size) in known_inline_literals(blob, pools) {
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

fn known_inline_literals(blob: &[u8], pools: &[Pool]) -> Vec<(usize, usize)> {
    let mut literals = Vec::new();
    for pool in pools {
        let mut search = 0;
        while let Some(found) = find(blob, &pool.prefix, search) {
            if let Some(start) = found.checked_sub(pool.prefix_offset).filter(|s| s % 4 == 0) {
                let bytes = &blob[start..blob.len().min(start + pool.size)];
                if hex::encode(Sha256::digest(bytes)) == pool.sha256 {
                    literals.push((start, pool.size));
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::macho::Segment;

    fn module(va: u64, size: u64) -> Image {
        let segment = Segment {
            name: "__TEXT_EXEC".into(),
            va,
            size,
            fileoff: 0,
            filesize: size,
            command_offset: 0,
            maxprot: 5,
            initprot: 5,
            sections: Vec::new(),
        };
        Image {
            kind: 0,
            commands: Vec::new(),
            segments: vec![segment],
        }
    }

    #[test]
    fn a_build_pool_hides_its_words_from_the_pc_relative_scan() {
        let va = 0xffff_fff0_0700_0000_u64;
        let mut pool: Vec<u8> = (0..64).collect();
        // B +0x20000, far past the module.
        pool[16..20].copy_from_slice(&0x1400_8000_u32.to_le_bytes());
        let mut code = 0xd503_201f_u32.to_le_bytes().repeat(4);
        code.extend_from_slice(&pool);
        let image = module(va, code.len() as u64);
        let read = |at: u64, size: u64| -> Result<&[u8]> { Ok(&code[(at - va) as usize..(at - va + size) as usize]) };
        let digest = hex::encode(Sha256::digest(&pool));
        let algorithms = pools(std::iter::empty(), read).unwrap();
        assert!(inline_data(std::iter::once(&image), &algorithms, read).is_err());
        let build = pools(std::iter::once((va + 16, 64, digest.as_str())), read).unwrap();
        let found = inline_data(std::iter::once(&image), &build, read).unwrap();
        assert_eq!(
            found.iter().map(|d| (d.va, d.size)).collect::<Vec<_>>(),
            [(va + 16, 64)]
        );
        assert!(pools(std::iter::once((va + 16, 64, "00")), read).is_err());
    }
}
