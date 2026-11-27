// SPDX-License-Identifier: GPL-3.0-or-later
//! Our kernel with the prelinked segments appended into its empty placeholder segments, and the sparse header index's
//! section added to `__PRELINK_TEXT`, as both prelinkers write them.

use object::LittleEndian as LE;
use object::macho::{MachHeader64, Section64, SectionFlags, SegmentCommand64, VmProt};
use object::{U32, U64, pod};

use super::{HEADER_SIZE, Image, LC_SEGMENT_64, SECTION_SIZE, SEGMENT_SIZE};
use crate::error::{Result, refused};

const VM_PAGE: u64 = 0x4000;

pub(crate) struct NativeImage<'a> {
    native: &'a Image,
    pub output: Vec<u8>,
    /// Where each appended segment's file bytes start: the 17 prelink pages the file at 4 KiB, the fileset one at
    /// 16 KiB.
    file_page: u64,
    appended: Vec<(&'static str, u64, u64)>,
}

impl<'a> NativeImage<'a> {
    pub fn new(native: &'a Image, data: &[u8], file_page: u64) -> Self {
        NativeImage {
            native,
            output: data.to_vec(),
            file_page,
            appended: Vec::new(),
        }
    }

    /// Appends `blob` at the next file page as segment `name`, whose empty native segment (with one section) is
    /// rewritten to map it; returns its file offset.
    pub fn append_segment(
        &mut self,
        name: &'static str,
        va: u64,
        blob: &[u8],
        size: u64,
        protection: u32,
    ) -> Result<usize> {
        if va % VM_PAGE != 0 || size % VM_PAGE != 0 || blob.len() as u64 > size {
            return refused("Coalesced segment must be whole VM pages");
        }
        let (start, end) = (u128::from(va), u128::from(va) + u128::from(size));
        let existing = self.native.segments.iter().map(|s| (s.va, s.size));
        if existing
            .chain(self.appended.iter().map(|&(_, va, size)| (va, size)))
            .any(|(s_va, s_size)| {
                s_size != 0 && start < u128::from(s_va) + u128::from(s_size) && u128::from(s_va) < end
            })
        {
            return refused("Coalesced segment collides with existing mapping");
        }
        let placeholder = self.native.segments.iter().find(|s| s.name == name);
        let Some(placeholder) = placeholder.filter(|p| p.size == 0 && p.sections.len() == 1) else {
            return refused(format!("Expected one empty native {name} segment/section"));
        };
        let fileoff = (self.output.len() as u64).next_multiple_of(self.file_page);
        let Ok(section_offset) = u32::try_from(fileoff) else {
            return refused(format!("{name} past a 32-bit file offset"));
        };
        self.output.resize(fileoff as usize, 0);
        self.output.extend_from_slice(blob);
        self.output.resize((fileoff + size) as usize, 0);
        let command = command_mut::<SegmentCommand64<LE>>(&mut self.output, placeholder.command_offset)?;
        command.vmaddr.set(LE, va);
        command.vmsize.set(LE, size);
        command.fileoff.set(LE, fileoff);
        command.filesize.set(LE, size);
        command.maxprot.set(LE, VmProt(protection));
        command.initprot.set(LE, VmProt(protection));
        let section = command_mut::<Section64<LE>>(&mut self.output, placeholder.sections[0].command_offset)?;
        section.addr.set(LE, va);
        section.size.set(LE, size);
        section.offset.set(LE, section_offset);
        self.appended.push((name, va, size));
        Ok(fileoff as usize)
    }

    /// Adds the `__macho_list` section, the sparse header index at file offset `at`, to each `__PRELINK_TEXT`
    /// command, in the header's padding: no payload offset moves.
    pub fn add_sparse_index_section(&mut self, va: u64, size: u64, at: usize) -> Result<()> {
        let Some(&(_, prelink_text, _)) = self.appended.iter().find(|(name, _, _)| *name == "__PRELINK_TEXT") else {
            return refused("No __PRELINK_TEXT appended");
        };
        let Ok(offset) = u32::try_from(at) else {
            return refused("Sparse index past a 32-bit file offset");
        };
        let mut rebuilt = Vec::new();
        for command in &self.native.commands {
            let mut blob = self.output[command.offset..command.offset + command.size].to_vec();
            if command.cmd == LC_SEGMENT_64 && super::name(blob[8..24].try_into().unwrap())? == "__PRELINK_TEXT" {
                let segment = command_mut::<SegmentCommand64<LE>>(&mut blob, 0)?;
                segment.cmdsize.set(LE, (command.size + SECTION_SIZE) as u32);
                segment.nsects.set(LE, 2);
                command_mut::<Section64<LE>>(&mut blob, SEGMENT_SIZE)?
                    .size
                    .set(LE, va - prelink_text);
                let section = Section64::<LE> {
                    sectname: padded(b"__macho_list"),
                    segname: padded(b"__PRELINK_TEXT"),
                    addr: U64::new(LE, va),
                    size: U64::new(LE, size),
                    offset: U32::new(LE, offset),
                    align: U32::new(LE, 3),
                    reloff: U32::new(LE, 0),
                    nreloc: U32::new(LE, 0),
                    flags: U32::new(LE, SectionFlags(0)),
                    reserved1: U32::new(LE, 0),
                    reserved2: U32::new(LE, 0),
                    reserved3: U32::new(LE, 0),
                };
                blob.extend_from_slice(pod::bytes_of(&section));
            }
            rebuilt.extend_from_slice(&blob);
        }
        let first_payload = self
            .native
            .segments
            .iter()
            .flat_map(|s| &s.sections)
            .filter(|s| s.size != 0 && s.fileoff != 0)
            .map(|s| s.fileoff as usize)
            .min();
        let header_end = HEADER_SIZE + rebuilt.len();
        if first_payload.is_none_or(|first| header_end > first) {
            return refused("Insufficient native Mach-O header padding for sparse index section");
        }
        command_mut::<MachHeader64<LE>>(&mut self.output, 0)?
            .sizeofcmds
            .set(LE, rebuilt.len() as u32);
        self.output[HEADER_SIZE..header_end].copy_from_slice(&rebuilt);
        Ok(())
    }
}

fn command_mut<T: object::Pod>(data: &mut [u8], at: usize) -> Result<&mut T> {
    match data.get_mut(at..).map(pod::from_bytes_mut::<T>) {
        Some(Ok((value, _))) => Ok(value),
        _ => refused(format!("Mach-O structure at {at:#x} outside image")),
    }
}

fn padded(name: &[u8]) -> [u8; 16] {
    let mut out = [0; 16];
    out[..name.len()].copy_from_slice(name);
    out
}
