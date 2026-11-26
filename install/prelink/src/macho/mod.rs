// SPDX-License-Identifier: GPL-3.0-or-later
//! 64-bit little-endian ARM64 Mach-O, read with the checks of `tools/common/macho.py`: every offset and size is
//! checked against the image, so nothing past this reader indexes outside it.

mod append;
mod fileset;
mod symbols;

pub(crate) use append::NativeImage;
pub(crate) use fileset::{fileset_entries, linkedit_data};
pub(crate) use symbols::{export_addresses, exports};

use object::macho::{self, LoadCommand, MachHeader64, Section64, SegmentCommand64};
use object::{BigEndian, LittleEndian as LE, Pod, ReadRef};

use crate::error::{Result, refused};

pub(crate) const MH_EXECUTE: u32 = macho::MH_EXECUTE.0;
pub(crate) const MH_KEXT_BUNDLE: u32 = macho::MH_KEXT_BUNDLE.0;
pub(crate) const MH_FILESET: u32 = macho::MH_FILESET.0;
pub(crate) const LC_SEGMENT_64: u32 = macho::LC_SEGMENT_64.0;
pub(crate) const LC_FUNCTION_STARTS: u32 = macho::LC_FUNCTION_STARTS.0;
pub(crate) const LC_DYLD_CHAINED_FIXUPS: u32 = macho::LC_DYLD_CHAINED_FIXUPS.0;
pub(crate) const HEADER_SIZE: usize = size_of::<MachHeader64<LE>>();
pub(crate) const SEGMENT_SIZE: usize = size_of::<SegmentCommand64<LE>>();
pub(crate) const SECTION_SIZE: usize = size_of::<Section64<LE>>();

/// The subtypes an ordinary image has; a fileset prelink passes its own.
pub(crate) const ORDINARY: &[u32] = &[0];

pub(crate) struct Command {
    pub cmd: u32,
    pub offset: usize,
    pub size: usize,
}

pub(crate) struct Section {
    pub va: u64,
    pub size: u64,
    pub fileoff: u32,
    pub command_offset: usize,
    pub flags: u32,
}

pub(crate) struct Segment {
    pub name: String,
    pub va: u64,
    pub size: u64,
    pub fileoff: u64,
    pub filesize: u64,
    pub command_offset: usize,
    pub maxprot: u32,
    pub initprot: u32,
    pub sections: Vec<Section>,
}

impl Segment {
    pub fn file_extent(&self) -> (u64, u64, u64) {
        (self.va, self.filesize, self.fileoff)
    }

    pub fn holds(&self, address: u64, length: u64) -> bool {
        self.va <= address
            && u128::from(address) + u128::from(length) <= u128::from(self.va) + u128::from(self.filesize)
    }
}

pub(crate) struct Image {
    pub kind: u32,
    pub commands: Vec<Command>,
    pub segments: Vec<Segment>,
}

impl Image {
    /// Reads the image whose header is at `at`, of one of `subtypes` (`None`: any ARM64).
    pub fn parse(data: &[u8], at: usize, subtypes: Option<&[u32]>) -> Result<Image> {
        if at.checked_add(HEADER_SIZE).is_none_or(|end| end > data.len()) {
            return refused("Mach-O header outside image");
        }
        let header: &MachHeader64<LE> = read(data, at)?;
        let subtype = header.cpusubtype.get(LE).0;
        if header.magic.get(BigEndian) != macho::MH_CIGAM_64
            || header.cputype.get(LE) != macho::CPU_TYPE_ARM64
            || subtypes.is_some_and(|s| !s.contains(&subtype))
        {
            return refused("Only ordinary little-endian ARM64 is supported");
        }
        let end = at + HEADER_SIZE + header.sizeofcmds.get(LE) as usize;
        if end > data.len() {
            return refused("Load commands outside image");
        }
        let mut image = Image {
            kind: header.filetype.get(LE).0,
            commands: Vec::new(),
            segments: Vec::new(),
        };
        let mut offset = at + HEADER_SIZE;
        for _ in 0..header.ncmds.get(LE) {
            if offset + 8 > end {
                return refused("Truncated load command");
            }
            let command: &LoadCommand<LE> = read(data, offset)?;
            let (cmd, size) = (command.cmd.get(LE).0, command.cmdsize.get(LE) as usize);
            if size < 8 || size % 8 != 0 || offset + size > end {
                return refused("Invalid load command extent");
            }
            image.commands.push(Command { cmd, offset, size });
            if cmd == LC_SEGMENT_64 {
                image.segments.push(segment(data, offset, size)?);
            }
            offset += size;
        }
        if offset != end {
            return refused("Load command size mismatch");
        }
        Ok(image)
    }

    pub fn segment(&self, name: &str) -> Result<&Segment> {
        match self.segments.iter().find(|s| s.name == name) {
            Some(segment) => Ok(segment),
            None => refused(format!("No {name} segment")),
        }
    }

    pub fn offset_of(&self, address: u64, length: u64, data_len: usize) -> Result<usize> {
        backed_offset(
            self.segments.iter().map(Segment::file_extent),
            address,
            length,
            data_len,
        )
    }
}

fn segment(data: &[u8], offset: usize, size: usize) -> Result<Segment> {
    if size < SEGMENT_SIZE {
        return refused("Truncated segment");
    }
    let command: &SegmentCommand64<LE> = read(data, offset)?;
    let (va, vmsize, filesize) = (command.vmaddr.get(LE), command.vmsize.get(LE), command.filesize.get(LE));
    let nsects = command.nsects.get(LE) as usize;
    let top = u128::from(va) + u128::from(vmsize);
    if Some(size) != nsects.checked_mul(SECTION_SIZE).map(|s| s + SEGMENT_SIZE) || filesize > vmsize || top > 1 << 64 {
        return refused("Invalid segment extent");
    }
    let mut sections = Vec::with_capacity(nsects);
    for index in 0..nsects {
        let at = offset + SEGMENT_SIZE + index * SECTION_SIZE;
        let section: &Section64<LE> = read(data, at)?;
        let (address, extent) = (section.addr.get(LE), section.size.get(LE));
        if extent != 0 && !(va <= address && u128::from(address) + u128::from(extent) <= top) {
            return refused("Section outside segment");
        }
        name(&section.sectname)?;
        name(&section.segname)?;
        sections.push(Section {
            va: address,
            size: extent,
            fileoff: section.offset.get(LE),
            command_offset: at,
            flags: section.flags.get(LE).0,
        });
    }
    Ok(Segment {
        name: name(&command.segname)?,
        va,
        size: vmsize,
        fileoff: command.fileoff.get(LE),
        filesize,
        command_offset: offset,
        maxprot: command.maxprot.get(LE).0,
        initprot: command.initprot.get(LE).0,
        sections,
    })
}

pub(crate) fn name(raw: &[u8; 16]) -> Result<String> {
    let end = raw.iter().rposition(|&b| b != 0).map_or(0, |i| i + 1);
    match std::str::from_utf8(&raw[..end]) {
        Ok(name) => Ok(name.to_owned()),
        Err(_) => refused("A Mach-O name that is not UTF-8"),
    }
}

pub(crate) fn read<T: Pod>(data: &[u8], at: usize) -> Result<&T> {
    match data.read_at::<T>(at as u64) {
        Ok(value) => Ok(value),
        Err(_) => refused(format!("Mach-O structure at {at:#x} outside image")),
    }
}

/// The file offset of `[address, address + length)`, which exactly one of the `(va, filesize, fileoff)` extents must
/// hold.
pub(crate) fn backed_offset(
    extents: impl IntoIterator<Item = (u64, u64, u64)>,
    address: u64,
    length: u64,
    data_len: usize,
) -> Result<usize> {
    let (start, end) = (u128::from(address), u128::from(address) + u128::from(length));
    let mut matches = extents
        .into_iter()
        .filter(|&(va, filesize, _)| u128::from(va) <= start && end <= u128::from(va) + u128::from(filesize))
        .map(|(va, _, fileoff)| u128::from(fileoff) + start - u128::from(va));
    match (matches.next(), matches.next()) {
        (Some(at), None) if at + u128::from(length) <= data_len as u128 => Ok(at as usize),
        _ => refused(format!("Ambiguous or unbacked address {address:#x}+{length:#x}")),
    }
}

pub(crate) fn u32_at(data: &[u8], at: usize) -> Result<u32> {
    match data.get(at..at + 4) {
        Some(b) => Ok(u32::from_le_bytes(b.try_into().unwrap())),
        None => refused(format!("Read at {at:#x} outside image")),
    }
}

pub(crate) fn u16_at(data: &[u8], at: usize) -> Result<u16> {
    match data.get(at..at + 2) {
        Some(b) => Ok(u16::from_le_bytes(b.try_into().unwrap())),
        None => refused(format!("Read at {at:#x} outside image")),
    }
}

pub(crate) fn u64_at(data: &[u8], at: usize) -> Result<u64> {
    match data.get(at..at + 8) {
        Some(b) => Ok(u64::from_le_bytes(b.try_into().unwrap())),
        None => refused(format!("Read at {at:#x} outside image")),
    }
}

pub(crate) fn put_u32(data: &mut [u8], at: usize, value: u32) -> Result<()> {
    match data.get_mut(at..at + 4) {
        Some(b) => {
            b.copy_from_slice(&value.to_le_bytes());
            Ok(())
        }
        None => refused(format!("Write at {at:#x} outside image")),
    }
}

pub(crate) fn put_u64(data: &mut [u8], at: usize, value: u64) -> Result<()> {
    match data.get_mut(at..at + 8) {
        Some(b) => {
            b.copy_from_slice(&value.to_le_bytes());
            Ok(())
        }
        None => refused(format!("Write at {at:#x} outside image")),
    }
}
