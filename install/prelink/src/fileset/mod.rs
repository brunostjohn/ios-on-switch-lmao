// SPDX-License-Identifier: GPL-3.0-or-later
//! The prelink of a fileset kernelcache's kexts (iPadOS 26 and 27) into our kernel from CI's bindings export, as
//! `translate/arm64e/xnu/prelink26/emit26.py --from-bindings` makes it, step for step.

mod bindings;
mod code;
mod fips;
mod fixups;
mod headers;
mod kernelcache;
mod layout;
mod output;
mod pins;
mod pointers;

use std::collections::{BTreeMap, BTreeSet};

use sha2::{Digest, Sha256};

use crate::error::{Error, Result, refused};
use crate::macho::{self, Image, ORDINARY, Segment};
use crate::sites::{Site, load_sites};
use bindings::Bindings;
use kernelcache::{Closure, Fileset, KERNEL};
use layout::{Layout, TEXT};
use pins::Pins;
pub use pins::fileset_pins;

const SPARSE_INDEX_MARKER: &str = "_kalloc_sparse_prelink_index_version";
const CUT_NULL: &str = "_switch_prelink_cut_null";

/// What a fileset prelink takes besides the two kernels.
pub struct FilesetInputs {
    bindings: Bindings,
    pins: Pins,
    sites: Vec<Site>,
}

impl FilesetInputs {
    /// `prelink-bindings.json` (CI's export), the release's pins of the build (`fileset_pins`), and the build's site
    /// tables (`kits/SwitchABI/kinds/<build>/sites/*.json`), which must be the pinned ones.
    pub fn new(bindings: &[u8], pins: &[u8], site_tables: &[&[u8]]) -> std::result::Result<Self, Error> {
        let bindings = Bindings::read(bindings)?;
        let pins = Pins::read(pins, &bindings.build)?;
        pins.check_sites(site_tables)?;
        let sites = load_sites(site_tables, &bindings.build)?;
        Ok(FilesetInputs { bindings, pins, sites })
    }
}

/// The prelinked kernel: `kernel` (MH_EXECUTE, unprelinked) with the kexts of `kernelcache` (the decompressed
/// MH_FILESET) the export selects. Refused, with no bytes, unless it is the kernel the pins name.
pub fn prelink_fileset(
    kernel: &[u8],
    kernelcache: &[u8],
    inputs: &FilesetInputs,
) -> std::result::Result<Vec<u8>, Error> {
    let native = Image::parse(kernel, 0, Some(ORDINARY))?;
    let kernelcache_sha256 = Sha256::digest(kernelcache);
    let bindings = &inputs.bindings;
    bindings.check_made_for(&hex::encode(kernelcache_sha256), &hex::encode(Sha256::digest(kernel)))?;
    let ours = macho::export_addresses(kernel, &native)?;
    check_sparse_index_abi(kernel, &native, ours.get(SPARSE_INDEX_MARKER).copied())?;
    let cache = Fileset::read(kernelcache)?;
    let closure = cache.closure(bindings)?;
    let layout = Layout::new(&cache, &closure, &native, bindings.slide)?;
    let p = Prelink {
        kernel_text: executable(&cache.entry(KERNEL)?.image.segments, true),
        our_text: executable(&native.segments, false),
        cache,
        closure,
        layout,
        bindings,
    };
    let mut blobs = p.layout.copy_segments(&p.cache, &p.closure)?;
    let translated = code::translate(&mut blobs[TEXT], &p.cache, &p.closure, &p.layout)?;
    code::write_sites(
        &mut blobs[TEXT],
        &inputs.sites,
        &p.cache,
        &p.closure,
        p.layout.text_lo(),
    )?;
    let mut unresolved = Unresolved::default();
    let pointers = pointers::bind(&mut blobs, &p, ours.get(CUT_NULL).copied(), &mut unresolved)?;
    code::rebind_branches(&mut blobs[TEXT], &p, &translated, &mut unresolved)?;
    unresolved.check()?;
    code::append_stubs(&mut blobs[TEXT], &p.layout, &translated.stubs)?;
    fips::write_macs(&mut blobs, &p)?;
    headers::rebase(&mut blobs, &p)?;
    headers::rebase_kmod_info(&mut blobs, &p, &pointers.written)?;
    let prelinked = output::emit(kernel, &native, blobs, &p, &pointers.kaslr, &kernelcache_sha256)?;
    inputs.pins.check_prelinked(&prelinked)?;
    Ok(prelinked)
}

pub(crate) struct Prelink<'a> {
    pub cache: Fileset<'a>,
    pub closure: Closure,
    pub layout: Layout,
    pub bindings: &'a Bindings,
    /// Apple's kernel's executable segments, and ours with a size.
    kernel_text: Vec<(u64, u128)>,
    our_text: Vec<(u64, u128)>,
}

impl Prelink<'_> {
    pub fn in_kernel_text(&self, address: u64) -> bool {
        holds(&self.kernel_text, address)
    }

    pub fn in_our_text(&self, address: u64) -> bool {
        holds(&self.our_text, address)
    }
}

fn executable(segments: &[Segment], empty_too: bool) -> Vec<(u64, u128)> {
    segments
        .iter()
        .filter(|s| s.initprot & 4 != 0 && (empty_too || s.size != 0))
        .map(|s| (s.va, u128::from(s.va) + u128::from(s.size)))
        .collect()
}

fn holds(ranges: &[(u64, u128)], address: u64) -> bool {
    ranges.iter().any(|&(lo, hi)| lo <= address && u128::from(address) < hi)
}

/// Apple kernel addresses the kexts use that the bindings don't bind, by kext: functions, then data.
#[derive(Default)]
pub(crate) struct Unresolved(BTreeMap<String, (BTreeSet<u64>, BTreeSet<u64>)>);

impl Unresolved {
    pub fn function(&mut self, kext: &str, target: u64) {
        self.0.entry(kext.to_owned()).or_default().0.insert(target);
    }

    pub fn data(&mut self, kext: &str, target: u64) {
        self.0.entry(kext.to_owned()).or_default().1.insert(target);
    }

    fn check(&self) -> Result<()> {
        if self.0.is_empty() {
            return Ok(());
        }
        let hex = |set: &BTreeSet<u64>| set.iter().map(|a| format!("{a:#x}")).collect::<Vec<_>>().join(", ");
        let lines: Vec<String> = self
            .0
            .iter()
            .map(|(kext, (functions, data))| format!("  {kext}: functions [{}], data [{}]", hex(functions), hex(data)))
            .collect();
        refused(format!(
            "kernel targets the kexts use that the bindings don't bind:\n{}",
            lines.join("\n")
        ))
    }
}

fn check_sparse_index_abi(kernel: &[u8], native: &Image, marker: Option<u64>) -> Result<()> {
    let version = match marker {
        Some(address) => macho::u32_at(kernel, native.offset_of(address, 4, kernel.len())?)?,
        None => 0,
    };
    if version != 1 {
        return refused("our kernel lacks the sparse prelink index ABI v1");
    }
    Ok(())
}
