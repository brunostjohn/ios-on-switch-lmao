// SPDX-License-Identifier: GPL-3.0-or-later
//! `installd.py arm64_slice`: a thin image as it is, or the one ARM64 slice of a universal file.

use crate::Error;
use crate::macho::{self, CPU_TYPE_ARM64};

#[derive(Clone, Copy)]
enum Endian {
    Big,
    Little,
}

fn universal(magic: &[u8]) -> Option<(Endian, bool)> {
    match magic {
        b"\xca\xfe\xba\xbe" => Some((Endian::Big, false)),
        b"\xbe\xba\xfe\xca" => Some((Endian::Little, false)),
        b"\xca\xfe\xba\xbf" => Some((Endian::Big, true)),
        b"\xbf\xba\xfe\xca" => Some((Endian::Little, true)),
        _ => None,
    }
}

fn u32_at(data: &[u8], at: usize, endian: Endian) -> u32 {
    let bytes: [u8; 4] = data[at..at + 4].try_into().expect("four bytes");
    match endian {
        Endian::Big => u32::from_be_bytes(bytes),
        Endian::Little => u32::from_le_bytes(bytes),
    }
}

fn u64_at(data: &[u8], at: usize, endian: Endian) -> u64 {
    let bytes: [u8; 8] = data[at..at + 8].try_into().expect("eight bytes");
    match endian {
        Endian::Big => u64::from_be_bytes(bytes),
        Endian::Little => u64::from_le_bytes(bytes),
    }
}

struct Arch {
    cpu: u32,
    subtype: u32,
    offset: u64,
    size: u64,
    align: u32,
}

fn arch(data: &[u8], at: usize, endian: Endian, wide: bool) -> Arch {
    let word = |i: usize| u32_at(data, at + i, endian);
    if wide {
        Arch {
            cpu: word(0),
            subtype: word(4),
            offset: u64_at(data, at + 8, endian),
            size: u64_at(data, at + 16, endian),
            align: word(24),
        }
    } else {
        Arch {
            cpu: word(0),
            subtype: word(4),
            offset: u64::from(word(8)),
            size: u64::from(word(12)),
            align: word(16),
        }
    }
}

pub fn arm64_slice(data: &[u8]) -> Result<&[u8], Error> {
    let Some((endian, wide)) = data.get(..4).and_then(universal) else {
        macho::parse(data)?;
        return Ok(data);
    };
    if data.len() < 8 {
        return Err(Error::Refused("truncated universal header"));
    }
    let count = u32_at(data, 4, endian) as u64;
    let entry = if wide { 32 } else { 20 };
    let end = 8 + count * entry;
    let length = data.len() as u64;
    if count == 0 || end > length {
        return Err(Error::Refused("universal architecture table outside image"));
    }
    let (mut ranges, mut selected) = (Vec::<(u64, u64)>::new(), Vec::new());
    for index in 0..count {
        let a = arch(data, (8 + index * entry) as usize, endian, wide);
        let (start, stop) = (a.offset, u128::from(a.offset) + u128::from(a.size));
        if a.align > 63
            || !a.offset.is_multiple_of(1u64 << a.align)
            || a.offset < end
            || a.size < 32
            || stop > u128::from(length)
        {
            return Err(Error::Refused("invalid universal slice extent or alignment"));
        }
        let stop = stop as u64;
        if ranges.iter().any(|&(s, e)| start < e && s < stop) {
            return Err(Error::Refused("overlapping universal slices"));
        }
        ranges.push((start, stop));
        if a.cpu == CPU_TYPE_ARM64 {
            let image = &data[start as usize..stop as usize];
            if macho::parse(image)?.cpusubtype != a.subtype {
                return Err(Error::Refused("universal and Mach-O CPU subtypes differ"));
            }
            selected.push(image);
        }
    }
    match selected[..] {
        [image] => Ok(image),
        _ => Err(Error::Refused("expected exactly one ARM64 universal slice")),
    }
}
