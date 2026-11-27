// SPDX-License-Identifier: GPL-3.0-or-later
//! `installd.py inject`: an `LC_LOAD_DYLIB` of the install policy written into the header padding, which must be
//! empty and end before the first file-backed section.

use crate::macho::{self, Image, u32_at};
use crate::{Error, LIBRARY, slice};

const MH_EXECUTE: u32 = 2;
const LC_LOAD_DYLIB: u32 = 0xC;
const LC_REQ_DYLD: u32 = 0x8000_0000;
/// Load, weak, re-export, lazy and upward dylib commands.
const DYLIB_COMMANDS: [u32; 5] = [
    LC_LOAD_DYLIB,
    0x18 | LC_REQ_DYLD,
    0x1F | LC_REQ_DYLD,
    0x20,
    0x23 | LC_REQ_DYLD,
];
/// Section types without file bytes: zerofill, GB zerofill, thread-local zerofill.
const ZEROFILL: [u32; 3] = [0x1, 0xC, 0x12];
const DYLIB_COMMAND_SIZE: usize = 24;

fn padding_end(data: &[u8], image: &Image, end: u64) -> Result<u64, Error> {
    let text: Vec<_> = image
        .segments
        .iter()
        .filter(|s| s.name == b"__TEXT" && s.fileoff == 0)
        .collect();
    let [text] = text[..] else {
        return Err(Error::Refused("expected one file-backed __TEXT segment at offset zero"));
    };
    let mut payload = Vec::new();
    for segment in &image.segments {
        let segment_end = u128::from(segment.fileoff) + u128::from(segment.filesize);
        if segment_end > data.len() as u128 {
            return Err(Error::Refused("segment outside image"));
        }
        if segment.fileoff != 0 && segment.filesize != 0 {
            payload.push(segment.fileoff);
        }
        for section in &segment.sections {
            if section.size == 0 || ZEROFILL.contains(&(section.flags & 0xFF)) {
                continue;
            }
            let (at, extent) = (u128::from(section.fileoff), u128::from(section.size));
            if at < u128::from(end) || !(u128::from(segment.fileoff) <= at && at + extent <= segment_end) {
                return Err(Error::Refused(
                    "file-backed section outside segment or overlapping load commands",
                ));
            }
            payload.push(section.fileoff);
        }
    }
    if payload.is_empty() {
        return Err(Error::Refused("installd has no file-backed sections"));
    }
    Ok(payload.into_iter().chain([text.filesize]).min().expect("not empty"))
}

/// The install policy's dependency commands, which must name it in UTF-8 within the command.
fn dependencies(data: &[u8], image: &Image) -> Result<Vec<u32>, Error> {
    let mut found = Vec::new();
    for command in image.commands.iter().filter(|c| DYLIB_COMMANDS.contains(&c.cmd)) {
        if command.length < DYLIB_COMMAND_SIZE {
            return Err(Error::Refused("truncated dylib command"));
        }
        let name_at = u32_at(data, command.offset + 8).expect("inside the command") as usize;
        if !(DYLIB_COMMAND_SIZE <= name_at && name_at < command.length) {
            return Err(Error::Refused("dylib name outside command"));
        }
        let name = &data[command.offset + name_at..command.offset + command.length];
        let terminator = name
            .iter()
            .position(|b| *b == 0)
            .ok_or(Error::Refused("unterminated dylib name"))?;
        let name =
            std::str::from_utf8(&name[..terminator]).map_err(|_| Error::Refused("a dylib name that is not UTF-8"))?;
        if name == LIBRARY {
            found.push(command.cmd);
        }
    }
    Ok(found)
}

fn dylib_command(length: usize) -> Vec<u8> {
    let mut command = Vec::with_capacity(length);
    for word in [LC_LOAD_DYLIB, length as u32, DYLIB_COMMAND_SIZE as u32, 0, 0, 0] {
        command.extend_from_slice(&word.to_le_bytes());
    }
    command.extend_from_slice(LIBRARY.as_bytes());
    command.resize(length, 0);
    command
}

/// The arm64 slice of `installd` with the install policy as a required dependency. An image that already has exactly
/// that one dependency comes back as it is.
pub fn inject(installd: &[u8]) -> Result<Vec<u8>, Error> {
    let data = slice::arm64_slice(installd)?;
    let image = macho::parse(data)?;
    if image.filetype != MH_EXECUTE || !matches!(image.cpusubtype & 0xFF_FFFF, 0 | 2) {
        return Err(Error::Refused("expected an ARM64 or ARM64e executable"));
    }
    let (count, size) = (u32_at(data, 16).unwrap(), u32_at(data, 20).unwrap());
    let end = 32 + u64::from(size);
    let limit = padding_end(data, &image, end)?;
    let found = dependencies(data, &image)?;
    if !found.is_empty() {
        if found != [LC_LOAD_DYLIB] {
            return Err(Error::Refused(
                "policy library must have exactly one required dependency",
            ));
        }
        return Ok(data.to_vec());
    }
    let length = (DYLIB_COMMAND_SIZE + LIBRARY.len() + 1).next_multiple_of(8);
    if end + length as u64 > limit {
        return Err(Error::Refused("installd has insufficient load-command padding"));
    }
    let padding = end as usize..end as usize + length;
    if data[padding.clone()].iter().any(|b| *b != 0) {
        return Err(Error::Refused("installd load-command padding is not empty"));
    }
    let mut patched = data.to_vec();
    patched[padding].copy_from_slice(&dylib_command(length));
    patched[16..20].copy_from_slice(&(count + 1).to_le_bytes());
    patched[20..24].copy_from_slice(&(size + length as u32).to_le_bytes());
    Ok(patched)
}
