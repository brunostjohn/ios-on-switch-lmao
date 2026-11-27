// SPDX-License-Identifier: GPL-3.0-or-later
//! The prelinked kernel as the layout reads it (native_xnu_layout.py parse_kernel, after DarwinBoot's ParseKernel):
//! its segments and its LC_UNIXTHREAD entry.

use crate::Error;

pub const PAGE: u64 = 0x1000;
pub const BLOCK: u64 = 0x20_0000;
pub const MIN_WINDOW: u64 = 0x1000_0000;
const MAX_WINDOW: u64 = 0x2_0000_0000;
const MAX_SEGMENTS: usize = 64;
const LC_SEGMENT_64: u32 = 0x19;
const LC_UNIXTHREAD: u32 = 5;
const VM_PROT_EXECUTE: u32 = 4;

pub struct Segment {
    pub vmaddr: u64,
    pub vmsize: u64,
    pub fileoff: u64,
    pub filesize: u64,
    protection: u32,
}

pub struct Kernel {
    pub segments: Vec<Segment>,
    pub base: u64,
    pub end: u64,
    pub entry: u64,
}

fn refused(why: &str) -> Error {
    Error::Layout(why.to_owned())
}

fn u32_at(data: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(data[at..at + 4].try_into().expect("four bytes"))
}

fn u64_at(data: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(data[at..at + 8].try_into().expect("eight bytes"))
}

pub fn valid_window(size: u64) -> Result<(), Error> {
    if !(MIN_WINDOW..=MAX_WINDOW).contains(&size) || !size.is_multiple_of(BLOCK) {
        return Err(refused("window must be 256 MiB..8 GiB in 2 MiB steps"));
    }
    Ok(())
}

pub fn bounded_end(start: u64, size: u64, label: &str) -> Result<u64, Error> {
    match start.checked_add(size) {
        Some(end) if size != 0 => Ok(end),
        _ => Err(Error::Layout(format!("{label} is empty or overflows uint64"))),
    }
}

fn segment(data: &[u8], cursor: usize, length: usize, count: usize) -> Result<Option<Segment>, Error> {
    if length < 72 || count == MAX_SEGMENTS {
        return Err(refused("invalid or excessive segment commands"));
    }
    if u32_at(data, cursor + 64) as usize > (length - 72) / 80 {
        return Err(refused("segment sections exceed command"));
    }
    let [vmaddr, vmsize, fileoff, filesize] = [24, 32, 40, 48].map(|at| u64_at(data, cursor + at));
    if vmsize == 0 {
        return Ok(None);
    }
    bounded_end(vmaddr, vmsize, "virtual segment")?;
    let len = data.len() as u64;
    if vmsize > MIN_WINDOW || filesize > vmsize || fileoff > len || filesize > len - fileoff {
        return Err(refused("invalid segment memory/file extent"));
    }
    Ok(Some(Segment {
        vmaddr,
        vmsize,
        fileoff,
        filesize,
        protection: u32_at(data, cursor + 60),
    }))
}

fn entry(data: &[u8], cursor: usize, length: usize, entry: Option<u64>) -> Result<u64, Error> {
    if entry.is_some() || length != 288 || (u32_at(data, cursor + 8), u32_at(data, cursor + 12)) != (6, 68) {
        return Err(refused("expected exactly one ARM64 LC_UNIXTHREAD state"));
    }
    Ok(u64_at(data, cursor + 272))
}

fn load_commands(data: &[u8], count: u32, end: usize) -> Result<(Vec<Segment>, u64), Error> {
    let (mut cursor, mut pc, mut segments) = (32, None, Vec::new());
    for _ in 0..count {
        if end - cursor < 8 {
            return Err(refused("truncated load command"));
        }
        let (command, length) = (u32_at(data, cursor), u32_at(data, cursor + 4) as usize);
        if length < 8 || length % 8 != 0 || length > end - cursor {
            return Err(refused("invalid load-command length"));
        }
        if command == LC_SEGMENT_64 {
            segments.extend(segment(data, cursor, length, segments.len())?);
        } else if command == LC_UNIXTHREAD {
            pc = Some(entry(data, cursor, length, pc)?);
        }
        cursor += length;
    }
    match pc {
        Some(pc) if cursor == end && !segments.is_empty() => Ok((segments, pc)),
        _ => Err(refused("incomplete load-command table, segments or entry")),
    }
}

pub fn parse(data: &[u8]) -> Result<Kernel, Error> {
    if !(32..=128 << 20).contains(&data.len()) {
        return Err(refused("kernel must be 32 bytes..128 MiB"));
    }
    let header = (u32_at(data, 0), u32_at(data, 4), u32_at(data, 8), u32_at(data, 12));
    if header != (0xFEED_FACF, 0x100_000C, 0, 2) {
        return Err(refused("expected little-endian ARM64 subtype-0 MH_EXECUTE"));
    }
    let size = u32_at(data, 20) as usize;
    if size > data.len() - 32 {
        return Err(refused("load-command table exceeds kernel file"));
    }
    let (segments, entry) = load_commands(data, u32_at(data, 16), 32 + size)?;
    let mut ordered: Vec<&Segment> = segments.iter().collect();
    ordered.sort_by_key(|s| s.vmaddr);
    if ordered
        .windows(2)
        .any(|pair| pair[0].vmaddr + pair[0].vmsize > pair[1].vmaddr)
    {
        return Err(refused("overlapping virtual segments"));
    }
    let (base, last) = (ordered[0].vmaddr, ordered[ordered.len() - 1]);
    let end = last.vmaddr + last.vmsize;
    let executable = segments
        .iter()
        .any(|s| s.vmaddr <= entry && entry < s.vmaddr + s.filesize && s.protection & VM_PROT_EXECUTE != 0);
    if !executable
        || entry % 4 != 0
        || base % PAGE != 0
        || base > u64::MAX - MIN_WINDOW
        || end - base > MIN_WINDOW - BLOCK
    {
        return Err(refused("invalid kernel base, span or executable file-backed entry"));
    }
    Ok(Kernel {
        segments,
        base,
        end,
        entry,
    })
}
