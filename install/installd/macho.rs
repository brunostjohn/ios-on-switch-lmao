// SPDX-License-Identifier: GPL-3.0-or-later
//! `tools/common/macho.py macho(data, subtypes=None)`: the header, load commands and segments of a 64-bit
//! little-endian ARM64 image, refused wherever the Python refuses it.

use crate::Error;

const MH_MAGIC_64: u32 = 0xFEED_FACF;
pub const CPU_TYPE_ARM64: u32 = 0x0100_000C;
const LC_SEGMENT_64: u32 = 0x19;

pub struct Section {
    pub fileoff: u64,
    pub size: u64,
    pub flags: u32,
}

pub struct Segment {
    pub name: Vec<u8>,
    pub fileoff: u64,
    pub filesize: u64,
    pub sections: Vec<Section>,
}

pub struct Command {
    pub cmd: u32,
    pub offset: usize,
    pub length: usize,
}

pub struct Image {
    pub filetype: u32,
    pub cpusubtype: u32,
    pub commands: Vec<Command>,
    pub segments: Vec<Segment>,
}

pub fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at.checked_add(4)?)?.try_into().ok()?))
}

fn u64_at(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(data.get(at..at.checked_add(8)?)?.try_into().ok()?))
}

/// A load command's fixed name field as `rstrip(b'\0').decode()` reads it: the bytes must be UTF-8.
fn name(field: &[u8]) -> Result<Vec<u8>, Error> {
    let end = field.iter().rposition(|b| *b != 0).map_or(0, |i| i + 1);
    std::str::from_utf8(&field[..end]).map_err(|_| Error::Refused("a segment or section name that is not UTF-8"))?;
    Ok(field[..end].to_vec())
}

pub fn parse(data: &[u8]) -> Result<Image, Error> {
    if data.len() < 32 {
        return Err(Error::Refused("Mach-O header outside image"));
    }
    let word = |at| u32_at(data, at).expect("inside the header");
    let (magic, cpu, cpusubtype, filetype, count, size) = (word(0), word(4), word(8), word(12), word(16), word(20));
    if magic != MH_MAGIC_64 || cpu != CPU_TYPE_ARM64 {
        return Err(Error::Refused("Only ordinary little-endian ARM64 is supported"));
    }
    let end = 32 + size as usize;
    if end > data.len() {
        return Err(Error::Refused("Load commands outside image"));
    }
    let (mut offset, mut commands, mut segments) = (32, Vec::new(), Vec::new());
    for _ in 0..count {
        if offset + 8 > end {
            return Err(Error::Refused("Truncated load command"));
        }
        let (cmd, length) = (word(offset), word(offset + 4) as usize);
        if length < 8 || length % 8 != 0 || offset + length > end {
            return Err(Error::Refused("Invalid load command extent"));
        }
        if cmd == LC_SEGMENT_64 {
            segments.push(segment(data, offset, length)?);
        }
        commands.push(Command { cmd, offset, length });
        offset += length;
    }
    if offset != end {
        return Err(Error::Refused("Load command size mismatch"));
    }
    Ok(Image {
        filetype,
        cpusubtype,
        commands,
        segments,
    })
}

fn segment(data: &[u8], offset: usize, length: usize) -> Result<Segment, Error> {
    if length < 72 {
        return Err(Error::Refused("Truncated segment"));
    }
    let quad = |at| u64_at(data, offset + at).expect("inside the command");
    let (va, vmsize, fileoff, filesize) = (quad(24), quad(32), quad(40), quad(48));
    let nsects = u32_at(data, offset + 64).expect("inside the command") as usize;
    if length as u64 != 72 + 80 * nsects as u64 || filesize > vmsize || u128::from(va) + u128::from(vmsize) > 1 << 64 {
        return Err(Error::Refused("Invalid segment extent"));
    }
    let seg_name = name(&data[offset + 8..offset + 24])?;
    let mut sections = Vec::with_capacity(nsects);
    for index in 0..nsects {
        let at = offset + 72 + index * 80;
        name(&data[at..at + 16])?;
        name(&data[at + 16..at + 32])?;
        let (address, extent) = (u64_at(data, at + 32).unwrap(), u64_at(data, at + 40).unwrap());
        let (address, extent, va_end) = (
            u128::from(address),
            u128::from(extent),
            u128::from(va) + u128::from(vmsize),
        );
        if extent != 0 && !(u128::from(va) <= address && address + extent <= va_end) {
            return Err(Error::Refused("Section outside segment"));
        }
        sections.push(Section {
            fileoff: u64::from(u32_at(data, at + 48).unwrap()),
            size: extent as u64,
            flags: u32_at(data, at + 64).unwrap(),
        });
    }
    Ok(Segment {
        name: seg_name,
        fileoff,
        filesize,
        sections,
    })
}
