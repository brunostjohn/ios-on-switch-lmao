// SPDX-License-Identifier: GPL-3.0-or-later
//! A fileset kernelcache (MH_FILESET), read as `prelink26/fileset.py` and `kcinfo.py` read it: its entries and their
//! segments, its prelink info, and the closure of the kexts the export names.

use std::collections::{BTreeSet, HashMap};

use super::bindings::Bindings;
use crate::error::{Result, refused};
use crate::info::{self, Dict, Value};
use crate::macho::{self, Image, LC_FUNCTION_STARTS, MH_FILESET};

pub(crate) const KERNEL: &str = "com.apple.kernel";
/// `_PrelinkExecutableLoadAddr` of a codeless kext in a fileset kernelcache (XNU's kOSKextCodelessKextLoadAddr).
const CODELESS_LOAD_ADDR: i128 = 0x7FFF_FFFF_FFFF_FFFF;

pub(crate) struct Entry {
    pub va: u64,
    pub fileoff: usize,
    pub image: Image,
}

pub(crate) struct Fileset<'a> {
    pub data: &'a [u8],
    pub image: Image,
    /// The first segment's address, which the chained fixups' targets count from.
    pub base: u64,
    entries: Vec<(String, Entry)>,
    by_id: HashMap<String, usize>,
    info: HashMap<String, Dict>,
}

/// The kexts with code the prelink copies, the kernel interfaces (`OSKernelResource`) and the codeless kexts, which
/// the prelink info alone carries.
pub(crate) struct Closure {
    pub selected: BTreeSet<String>,
    pub interfaces: BTreeSet<String>,
    pub codeless: BTreeSet<String>,
}

impl<'a> Fileset<'a> {
    pub fn read(data: &'a [u8]) -> Result<Self> {
        let image = Image::parse(data, 0, None)?;
        if image.kind != MH_FILESET {
            return refused("Expected an MH_FILESET kernelcache");
        }
        let Some(base) = image.segments.first().map(|s| s.va) else {
            return refused("A kernelcache without segments");
        };
        let (mut entries, mut by_id) = (Vec::new(), HashMap::new());
        for entry in macho::fileset_entries(data, &image)? {
            let Ok(fileoff) = usize::try_from(entry.fileoff) else {
                return refused(format!("{}: outside the kernelcache", entry.id));
            };
            let entry_image = Image::parse(data, fileoff, None)?;
            if by_id.insert(entry.id.clone(), entries.len()).is_some() {
                return refused(format!("{}: two fileset entries", entry.id));
            }
            let entry_data = Entry {
                va: entry.va,
                fileoff,
                image: entry_image,
            };
            entries.push((entry.id, entry_data));
        }
        let info = prelink_info(data, &image)?;
        Ok(Fileset {
            data,
            image,
            base,
            entries,
            by_id,
            info,
        })
    }

    /// Every fileset entry, in load command order.
    pub fn entries(&self) -> impl Iterator<Item = (&str, &Entry)> {
        self.entries.iter().map(|(id, entry)| (id.as_str(), entry))
    }

    pub fn entry(&self, id: &str) -> Result<&Entry> {
        match self.by_id.get(id) {
            Some(&index) => Ok(&self.entries[index].1),
            None => refused(format!("{id}: no fileset entry")),
        }
    }

    pub fn info(&self, id: &str) -> Result<&Dict> {
        match self.info.get(id) {
            Some(entry) => Ok(entry),
            None => refused(format!("{id}: not in the kernelcache's prelink info")),
        }
    }

    /// The file offset of `address`, in the first segment whose file bytes hold it (`Fileset.va2off`).
    pub fn offset(&self, address: u64) -> Result<usize> {
        let found = self
            .image
            .segments
            .iter()
            .find(|s| s.va <= address && u128::from(address) < u128::from(s.va) + u128::from(s.filesize));
        match found.and_then(|s| usize::try_from(s.fileoff + (address - s.va)).ok()) {
            Some(at) => Ok(at),
            None => refused(format!("{address:#x}: not in the kernelcache's file")),
        }
    }

    pub fn bytes(&self, address: u64, length: u64) -> Result<&'a [u8]> {
        let at = self.offset(address)?;
        match usize::try_from(length)
            .ok()
            .and_then(|l| self.data.get(at..at.checked_add(l)?))
        {
            Some(bytes) => Ok(bytes),
            None => refused(format!("{address:#x}+{length:#x}: past the kernelcache's end")),
        }
    }

    /// `KC.closure`: the export's roots and their dependencies, without the cut ones.
    pub fn closure(&self, bindings: &Bindings) -> Result<Closure> {
        let missing: Vec<&str> = bindings
            .roots
            .iter()
            .filter(|r| !self.info.contains_key(*r))
            .map(String::as_str)
            .collect();
        if !missing.is_empty() {
            return refused(format!("roots this kernelcache does not ship: {}", missing.join(", ")));
        }
        let (mut selected, mut interfaces) = (BTreeSet::new(), BTreeSet::new());
        let mut pending = bindings.roots.clone();
        while let Some(id) = pending.pop() {
            if selected.contains(&id) || interfaces.contains(&id) {
                continue;
            }
            let entry = self.info(&id)?;
            if entry.get("OSKernelResource").is_some_and(Value::truthy) {
                interfaces.insert(id);
                continue;
            }
            match entry.get("OSBundleLibraries") {
                Some(Value::Dict(libraries)) => pending.extend(
                    libraries
                        .keys()
                        .filter(|dependency| !bindings.is_cut(&id, dependency))
                        .map(str::to_owned),
                ),
                Some(other) if other.truthy() => {
                    return refused(format!("{id}: OSBundleLibraries is not a dictionary"));
                }
                _ => {}
            }
            selected.insert(id);
        }
        let codeless: BTreeSet<String> = selected
            .iter()
            .filter(|id| !self.by_id.contains_key(*id))
            .cloned()
            .collect();
        for id in &codeless {
            let entry = self.info(id)?;
            let loaded = entry
                .get("_PrelinkExecutableLoadAddr")
                .is_some_and(|v| *v != Value::Integer(CODELESS_LOAD_ADDR));
            if entry.get("_PrelinkKmodInfo").is_some_and(Value::truthy)
                || loaded
                || !entry.get("IOKitPersonalities").is_some_and(Value::truthy)
            {
                return refused(format!(
                    "a kext in no fileset entry must be codeless with personalities: {id}"
                ));
            }
        }
        selected.retain(|id| !codeless.contains(id));
        Ok(Closure {
            selected,
            interfaces,
            codeless,
        })
    }

    /// The entry's `LC_FUNCTION_STARTS` as addresses: ULEB128 deltas from its `__TEXT`, up to a zero delta.
    pub fn function_starts(&self, entry: &Entry) -> Result<Vec<u64>> {
        let Some(text) = entry.image.segments.iter().find(|s| s.name == "__TEXT") else {
            return refused("A kext without a __TEXT segment");
        };
        let mut starts = Vec::new();
        for command in entry.image.commands.iter().filter(|c| c.cmd == LC_FUNCTION_STARTS) {
            let (offset, size) = macho::linkedit_data(self.data, command)?;
            let (mut address, mut at) = (text.va, offset);
            while at < offset + size {
                let (delta, next) = uleb128(self.data, at)?;
                at = next;
                if delta == 0 {
                    break;
                }
                let Some(sum) = address.checked_add(delta) else {
                    return refused("A function start past the 64-bit address space");
                };
                address = sum;
                starts.push(address);
            }
        }
        Ok(starts)
    }

    /// `_PrelinkKmodInfo` as the 64-bit address `& M64` gives.
    pub fn kmod_info(&self, id: &str) -> Result<u64> {
        address(self.info(id)?, id, "_PrelinkKmodInfo")
    }

    pub fn load_address(&self, id: &str) -> Result<u64> {
        address(self.info(id)?, id, "_PrelinkExecutableLoadAddr")
    }
}

fn address(entry: &Dict, id: &str, key: &str) -> Result<u64> {
    match entry.get(key) {
        Some(&Value::Integer(value)) => Ok(value as u64),
        _ => refused(format!("{id}: no integer {key}")),
    }
}

/// A ULEB128 at `at`, which may run past the command's data as Python's reader lets it, and the offset after it.
fn uleb128(data: &[u8], mut at: usize) -> Result<(u64, usize)> {
    let (mut value, mut shift) = (0u64, 0u32);
    loop {
        let Some(&byte) = data.get(at) else {
            return refused("Function starts past the kernelcache's end");
        };
        at += 1;
        let bits = u64::from(byte & 0x7F);
        if bits != 0 {
            if shift >= 64 || bits.leading_zeros() < shift {
                return refused("A function start delta wider than 64 bits");
            }
            value |= bits << shift;
        }
        shift = shift.saturating_add(7);
        if byte & 0x80 == 0 {
            return Ok((value, at));
        }
    }
}

fn prelink_info(data: &[u8], image: &Image) -> Result<HashMap<String, Dict>> {
    let segment = image.segment("__PRELINK_INFO")?;
    let Some(section) = segment.sections.first() else {
        return refused("__PRELINK_INFO without a section");
    };
    let at = section.fileoff as usize;
    let Some(blob) = usize::try_from(section.size)
        .ok()
        .and_then(|size| data.get(at..at.checked_add(size)?))
    else {
        return refused("Prelink info outside the kernelcache");
    };
    let Value::Dict(mut metadata) = info::unserialize_plist(blob)? else {
        return refused("Prelink info is not a dictionary");
    };
    let Some(Value::Array(list)) = metadata.remove("_PrelinkInfoDictionary") else {
        return refused("Prelink info has no _PrelinkInfoDictionary");
    };
    let mut entries = HashMap::with_capacity(list.len());
    for entry in list {
        let Value::Dict(entry) = entry else {
            return refused("A prelink info entry that is not a dictionary");
        };
        let id = entry.string("CFBundleIdentifier")?.to_owned();
        if entries.insert(id, entry).is_some() {
            return refused("Duplicate prelink identifier");
        }
    }
    Ok(entries)
}

/// Every kext's mapped segments by address, for `owner`. Sorted as Python sorts its `(va, end, kext, segment)`
/// tuples, so that overlapping extents resolve to the same owner.
pub(crate) struct Extents(Vec<(u64, u128, String, String)>);

impl Extents {
    pub fn new(cache: &Fileset) -> Self {
        let mut extents = Vec::new();
        for (id, entry) in cache.entries() {
            for s in &entry.image.segments {
                if s.size != 0 && s.name != "__LINKEDIT" {
                    let end = u128::from(s.va) + u128::from(s.size);
                    extents.push((s.va, end, id.to_owned(), s.name.clone()));
                }
            }
        }
        extents.sort_unstable();
        Extents(extents)
    }

    /// The kext whose segment holds `address`: the last one starting at or below it.
    pub fn owner(&self, address: u64) -> Option<&str> {
        let index = self.0.partition_point(|e| e.0 <= address);
        let (_, end, kext, _) = self.0.get(index.checked_sub(1)?)?;
        (u128::from(address) < *end).then_some(kext.as_str())
    }
}
