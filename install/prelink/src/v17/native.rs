// SPDX-License-Identifier: GPL-3.0-or-later
//! `emit_prelink.apply`: the native kernel with its exports bound, the coalesced segments and the prelink info
//! appended, the KCID, and the sparse index's section in the header.

use std::collections::{BTreeSet, HashMap};

use object::LittleEndian as LE;
use object::macho::{MachHeader64, Section64, SectionFlags, SegmentCommand64, VmProt};
use object::{U32, U64, pod};
use sha2::{Digest, Sha256};

use super::coalesced::VM_PAGE;
use super::entries::prelink_info;
use super::plan::Plan;
use crate::error::{Result, refused};
use crate::macho::{self, Image, LC_SEGMENT_64, MH_EXECUTE, ORDINARY, SECTION_SIZE, SEGMENT_SIZE};

const KCID_DOMAIN: &[u8] = b"SWITCH-MEMORY-V21-KCID\0";
const SPARSE_INDEX_MARKER: &str = "_kalloc_sparse_prelink_index_version";
const EARLY_SECURITY_INIT_MARKER: &str = "_native_prelink_security_init_version";
const FILE_PAGE: u64 = 4096;

pub(crate) fn apply(plan: Plan, native_data: &[u8], original_sha256: &[u8; 32]) -> Result<Vec<u8>> {
    let native = Image::parse(native_data, 0, Some(ORDINARY))?;
    if native.kind != MH_EXECUTE {
        return refused("Expected MH_EXECUTE kernel images");
    }
    let exported = macho::exports(native_data, &native)?;
    let (addresses, stubs) = bind(&native, &exported, &plan)?;
    check_native_abi(native_data, &native, &exported, plan.early_security_init)?;
    let mut image = NativeImage {
        native: &native,
        output: native_data.to_vec(),
        appended: Vec::new(),
    };
    let mut base = None;
    for segment in &plan.image.segments {
        let at = segment.fileoff as usize;
        let blob = &plan.image.output[at..at + segment.size as usize];
        let fileoff = image.append_segment(segment.name, segment.va, blob, segment.size, segment.protection)?;
        let Some(this_base) = fileoff.checked_sub(at) else {
            return refused("Coalesced segments are not contiguous");
        };
        if *base.get_or_insert(this_base) != this_base {
            return refused("Coalesced segments are not contiguous");
        }
    }
    let Some(base) = base else {
        return refused("No coalesced segments");
    };
    for &(at, group) in &plan.kernel_sites {
        image.output[base + at..base + at + 8].copy_from_slice(&addresses[group].to_le_bytes());
    }
    for (cut, address) in plan.cut_sites.iter().zip(&stubs) {
        image.output[base + cut.at..base + cut.at + 8].copy_from_slice(&address.to_le_bytes());
    }
    let kcid = Sha256::new()
        .chain_update(KCID_DOMAIN)
        .chain_update(native_data)
        .chain_update(original_sha256)
        .finalize();
    let xml = prelink_info(plan.entries, plan.kaslr_offsets, &kcid[..16])?;
    let ends: Option<Vec<u64>> = native
        .segments
        .iter()
        .filter(|s| s.size != 0)
        .map(|s| s.va.checked_add(s.size))
        .collect();
    let Some(info_va) = ends
        .and_then(|e| e.into_iter().max())
        .and_then(|top| top.checked_next_multiple_of(VM_PAGE))
    else {
        return refused("No address above the native kernel's segments for the prelink info");
    };
    let xml_size = (xml.len() as u64).next_multiple_of(VM_PAGE);
    image.append_segment("__PRELINK_INFO", info_va, &xml, xml_size, 3)?;
    image.add_sparse_index_section(plan.index, base)?;
    Image::parse(&image.output, 0, Some(ORDINARY))?;
    Ok(image.output)
}

/// Each kernel target's native address and each cut's stub's, with the checks that the rebased extents miss the
/// native segments.
fn bind(native: &Image, exported: &HashMap<u64, Vec<String>>, plan: &Plan) -> Result<(Vec<u64>, Vec<u64>)> {
    let mut by_name: HashMap<&str, BTreeSet<u64>> = HashMap::new();
    for (&address, names) in exported {
        for name in names {
            by_name.entry(name).or_default().insert(address);
        }
    }
    let mut addresses = Vec::with_capacity(plan.targets.len());
    for (target, names) in &plan.targets {
        let choices: BTreeSet<u64> = names
            .iter()
            .flat_map(|n| by_name.get(n.as_str()))
            .flatten()
            .copied()
            .collect();
        let [replacement] = choices.iter().copied().collect::<Vec<_>>()[..] else {
            return refused(format!(
                "Kernel target {target:#x} has {} native address candidates: {names:?}",
                choices.len()
            ));
        };
        if !native
            .segments
            .iter()
            .any(|s| s.va <= replacement && u128::from(replacement) < u128::from(s.va) + u128::from(s.size))
        {
            return refused("Native export is outside mapped kernel segments");
        }
        addresses.push(replacement);
    }
    let mut stubs = Vec::with_capacity(plan.cut_sites.len());
    for cut in &plan.cut_sites {
        let found = cut.stub.and_then(|stub| by_name.get(stub)).filter(|a| a.len() == 1);
        let Some(found) = found else {
            return refused(format!(
                "Cut {} -> {} needs one native stub {:?}",
                cut.component, cut.dependency, cut.stub
            ));
        };
        stubs.push(*found.first().unwrap());
    }
    for &(va, size) in &plan.extents {
        let (start, end) = (u128::from(va), u128::from(va) + u128::from(size));
        if let Some(s) = native
            .segments
            .iter()
            .find(|s| s.size != 0 && start < u128::from(s.va) + u128::from(s.size) && u128::from(s.va) < end)
        {
            return refused(format!("Rebased component collides with native {}", s.name));
        }
    }
    Ok((addresses, stubs))
}

fn check_native_abi(
    data: &[u8],
    native: &Image,
    exported: &HashMap<u64, Vec<String>>,
    early_security_init: bool,
) -> Result<()> {
    let version = |marker: &str| -> Result<Option<u32>> {
        let found: Vec<u64> = exported
            .iter()
            .filter(|(_, n)| n.iter().any(|n| n == marker))
            .map(|(&a, _)| a)
            .collect();
        let [address] = found[..] else {
            return Ok(None);
        };
        Ok(Some(macho::u32_at(data, native.offset_of(address, 4, data.len())?)?))
    };
    match version(SPARSE_INDEX_MARKER)? {
        None => return refused("Native kernel lacks sparse-prelink index ABI"),
        Some(1) => {}
        Some(_) => return refused("Unsupported native sparse-prelink index ABI"),
    }
    if early_security_init {
        match version(EARLY_SECURITY_INIT_MARKER)? {
            None => return refused("Native kernel lacks early security initializer ABI"),
            Some(1) => {}
            Some(_) => return refused("Unsupported early security initializer ABI"),
        }
    }
    Ok(())
}

struct NativeImage<'a> {
    native: &'a Image,
    output: Vec<u8>,
    appended: Vec<(&'static str, u64, u64)>,
}

impl NativeImage<'_> {
    /// Appends `blob` at the next file page as segment `name`, whose empty native segment (with one section) is
    /// rewritten to map it; returns its file offset.
    fn append_segment(
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
        let fileoff = (self.output.len() as u64).next_multiple_of(FILE_PAGE);
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

    /// Adds the `__macho_list` section, the sparse header index, to each `__PRELINK_TEXT` command, in the header's
    /// padding: no payload offset moves.
    fn add_sparse_index_section(&mut self, (va, size, at): (u64, u64, usize), base: usize) -> Result<()> {
        let Some(&(_, prelink_text, _)) = self.appended.iter().find(|(name, _, _)| *name == "__PRELINK_TEXT") else {
            return refused("No __PRELINK_TEXT appended");
        };
        let Ok(offset) = u32::try_from(base + at) else {
            return refused("Sparse index past a 32-bit file offset");
        };
        let mut rebuilt = Vec::new();
        for command in &self.native.commands {
            let mut blob = self.output[command.offset..command.offset + command.size].to_vec();
            if command.cmd == LC_SEGMENT_64 && macho::name(blob[8..24].try_into().unwrap())? == "__PRELINK_TEXT" {
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
        let header_end = macho::HEADER_SIZE + rebuilt.len();
        if first_payload.is_none_or(|first| header_end > first) {
            return refused("Insufficient native Mach-O header padding for sparse index section");
        }
        command_mut::<MachHeader64<LE>>(&mut self.output, 0)?
            .sizeofcmds
            .set(LE, rebuilt.len() as u32);
        self.output[macho::HEADER_SIZE..header_end].copy_from_slice(&rebuilt);
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
