// SPDX-License-Identifier: GPL-3.0-or-later
//! The coalesced segments back to back, as `apply` appends them to the native kernel (each a whole number of VM
//! pages, so contiguous there too): an offset in `output` is one in the prelinked kernel less the first segment's.

use super::closure::{Closure, Kernelcache, address};
use super::slid;
use crate::error::{Result, refused};
use crate::macho::{self, backed_offset};

/// XNU's VM page: the kernelcache's segments are mapped and freed as whole 16 KiB pages.
pub(crate) const VM_PAGE: u64 = 0x4000;

/// The kernelcache's coalesced segments and the component segments each holds.
const COALESCED: [(&str, &str); 4] = [
    ("__PRELINK_TEXT", "__TEXT"),
    ("__PLK_TEXT_EXEC", "__TEXT_EXEC"),
    ("__PLK_DATA_CONST", "__DATA_CONST"),
    ("__PRELINK_DATA", "__DATA"),
];

/// `struct kmod_info` (`#pragma pack(4)`): its size and the offset of `address`.
const KMOD_INFO_SIZE: u64 = 196;
const KMOD_INFO_ADDRESS: u64 = 156;

pub(crate) struct NewSegment {
    pub name: &'static str,
    pub va: u64,
    pub size: u64,
    pub fileoff: u64,
    pub protection: u32,
}

#[derive(Default)]
pub(crate) struct Coalesced {
    pub output: Vec<u8>,
    pub segments: Vec<NewSegment>,
    /// Every original-only write after the copies, as `(offset, length)`.
    pub writes: Vec<(usize, usize)>,
}

impl Coalesced {
    fn append_segment(&mut self, name: &'static str, va: u64, blob: Vec<u8>, protection: u32) -> Result<()> {
        let size = blob.len() as u64;
        if va % VM_PAGE != 0 || size % VM_PAGE != 0 {
            return refused("Coalesced segment must be whole VM pages");
        }
        let (start, end) = (u128::from(va), u128::from(va) + u128::from(size));
        if self
            .segments
            .iter()
            .any(|s| s.size != 0 && start < u128::from(s.va) + u128::from(s.size) && u128::from(s.va) < end)
        {
            return refused("Coalesced segment collides with existing mapping");
        }
        let fileoff = self.output.len() as u64;
        self.output.extend_from_slice(&blob);
        self.segments.push(NewSegment {
            name,
            va,
            size,
            fileoff,
            protection,
        });
        Ok(())
    }

    pub fn segment(&self, name: &str) -> &NewSegment {
        self.segments.iter().find(|s| s.name == name).unwrap()
    }

    pub fn new_offset(&self, address: u64, length: u64) -> Result<usize> {
        let extents = self.segments.iter().map(|s| (s.va, s.size, s.fileoff));
        backed_offset(extents, address, length, self.output.len())
    }

    pub fn u32_at(&self, at: usize) -> Result<u32> {
        macho::u32_at(&self.output, at)
    }

    pub fn write(&mut self, at: usize, bytes: &[u8]) {
        self.output[at..at + bytes.len()].copy_from_slice(bytes);
        self.writes.push((at, bytes.len()));
    }
}

/// The coalesced segments at their slid addresses, holding the selected components' segments and the kernel
/// interfaces' (packed kernel-resource records, copied as read-only metadata).
pub(crate) fn copy_segments(cache: &Kernelcache, closure: &Closure, slide: i64) -> Result<Coalesced> {
    let mut image = Coalesced::default();
    for (coalesced, component) in COALESCED {
        let source = cache.image.segment(coalesced)?;
        let mut blob = vec![0; source.size as usize];
        for segment in closure.modules.iter().flat_map(|(_, m)| &m.segments) {
            if segment.name != component || segment.filesize == 0 {
                continue;
            }
            let begin = i128::from(segment.va) - i128::from(source.va);
            if begin < 0 || begin + i128::from(segment.size) > blob.len() as i128 {
                return refused("Component is outside original coalesced range");
            }
            let at = cache.image.offset_of(segment.va, segment.filesize, cache.data.len())?;
            let (begin, length) = (begin as usize, segment.filesize as usize);
            blob[begin..begin + length].copy_from_slice(&cache.data[at..at + length]);
        }
        if coalesced == "__PRELINK_TEXT" {
            copy_kernel_interfaces(&mut blob, source.va, cache, closure)?;
        }
        image.append_segment(coalesced, slid(source.va, slide)?, blob, source.initprot)?;
    }
    Ok(image)
}

fn copy_kernel_interfaces(blob: &mut [u8], base: u64, cache: &Kernelcache, closure: &Closure) -> Result<()> {
    for identifier in &closure.interfaces {
        let entry = cache.entry(identifier)?;
        let va = entry_address(entry, "_PrelinkExecutableLoadAddr")?;
        let length = entry_address(entry, "_PrelinkExecutableSize")?;
        let begin = i128::from(va) - i128::from(base);
        let at = cache.image.offset_of(va, length, cache.data.len())?;
        if begin < 0 || begin + i128::from(length) > blob.len() as i128 {
            return refused("Kernel interface outside readonly prelink range");
        }
        let (begin, length) = (begin as usize, length as usize);
        blob[begin..begin + length].copy_from_slice(&cache.data[at..at + length]);
    }
    Ok(())
}

fn entry_address(entry: &crate::info::Dict, key: &str) -> Result<u64> {
    match entry.get(key) {
        Some(value) => address(value),
        None => refused(format!("Prelink info entry without {key}")),
    }
}

/// Each component's Mach-O header and load commands at its slid addresses: the cache's pointer list does not hold
/// them. Section flags and offsets stay as they are.
pub(crate) fn rebase_headers(image: &mut Coalesced, cache: &Kernelcache, closure: &Closure, slide: i64) -> Result<()> {
    for (identifier, module) in &closure.modules {
        let old_header = entry_address(cache.entry(identifier)?, "_PrelinkExecutableLoadAddr")?;
        let original_header = cache.image.offset_of(old_header, 32, cache.data.len())?;
        let new_header = image.new_offset(slid(old_header, slide)?, 32)?;
        for segment in &module.segments {
            if segment.va != 0 {
                let at = new_header + segment.command_offset - original_header;
                image.write(at + 24, &slid(segment.va, slide)?.to_le_bytes());
            }
            for section in segment.sections.iter().filter(|s| s.va != 0) {
                let at = new_header + section.command_offset - original_header;
                image.write(at + 32, &slid(section.va, slide)?.to_le_bytes());
            }
        }
    }
    Ok(())
}

/// Each component's `kmod_info.address` slid: OSKext's boot path slides it apart from the coalesced pointer table.
pub(crate) fn rebase_kmod_info(
    image: &mut Coalesced,
    cache: &Kernelcache,
    closure: &Closure,
    slide: i64,
) -> Result<()> {
    for identifier in &closure.components {
        let entry = cache.entry(identifier)?;
        let va = entry_address(entry, "_PrelinkKmodInfo")?;
        let at = cache.image.offset_of(va, KMOD_INFO_SIZE, cache.data.len())?;
        let header = macho::u64_at(cache.data, at + KMOD_INFO_ADDRESS as usize)?;
        if header == 0 {
            continue;
        }
        if header != entry_address(entry, "_PrelinkExecutableLoadAddr")? {
            return refused("Original kmod address disagrees with its component header");
        }
        let site = image.new_offset(slid(i128::from(va) + i128::from(KMOD_INFO_ADDRESS), slide)?, 8)?;
        image.write(site, &slid(header, slide)?.to_le_bytes());
    }
    Ok(())
}

/// The components' slid header addresses, sorted, at the end of `__PRELINK_TEXT`'s padding: `(va, size, offset)`.
pub(crate) fn sparse_index(
    image: &mut Coalesced,
    cache: &Kernelcache,
    closure: &Closure,
    slide: i64,
) -> Result<(u64, u64, usize)> {
    let mut headers = Vec::with_capacity(closure.components.len());
    for identifier in &closure.components {
        headers.push(slid(
            entry_address(cache.entry(identifier)?, "_PrelinkExecutableLoadAddr")?,
            slide,
        )?);
    }
    headers.sort_unstable();
    let prelink_text = image.segment("__PRELINK_TEXT");
    let size = headers.len() as u64 * 8;
    let va = (i128::from(prelink_text.va) + i128::from(prelink_text.size) - i128::from(size)) & !7;
    let mut occupied: Vec<i128> = Vec::new();
    for segment in closure
        .modules
        .iter()
        .flat_map(|(_, m)| &m.segments)
        .filter(|s| s.name == "__TEXT")
    {
        occupied.push(i128::from(segment.va) + i128::from(slide) + i128::from(segment.size));
    }
    for identifier in &closure.interfaces {
        let entry = cache.entry(identifier)?;
        let (load, length) = (
            entry_address(entry, "_PrelinkExecutableLoadAddr")?,
            entry_address(entry, "_PrelinkExecutableSize")?,
        );
        occupied.push(i128::from(load) + i128::from(slide) + i128::from(length));
    }
    let Some(&occupied_end) = occupied.iter().max() else {
        return refused("No component headers for the sparse header index");
    };
    if va < occupied_end {
        return refused("No readonly prelink padding for the sparse header index");
    }
    let va = slid(va, 0)?;
    let at = image.new_offset(va, size)?;
    let bytes: Vec<u8> = headers.iter().flat_map(|h| h.to_le_bytes()).collect();
    image.write(at, &bytes);
    Ok((va, size, at))
}
