// SPDX-License-Identifier: GPL-3.0-or-later
//! iOS 17's prelink: Apple's original kexts, from the kernelcache, slid and coalesced into our linked kernel, as
//! `tools/prelink/emit_prelink.py` makes it (its `plan` and `apply` in one pass).

mod closure;
mod coalesced;
mod entries;
mod inventory;
mod native;
mod plan;

use std::collections::{BTreeMap, BTreeSet, HashMap};

use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::error::{Error, Result, json, refused};
use crate::pyint;
use crate::sites::Site;
use closure::{Extent, Kernelcache};
#[cfg(test)]
pub(crate) use inventory::pc_relative_target;

/// The native kernel's stub, eight zero bytes, that a cut dependency's pointers are bound to.
const CUT_STUB: &str = "_switch_prelink_cut_null";

/// `prelink.json`: what the release carries beside the unprelinked kernel (`//install:kernel_inputs`).
#[derive(Deserialize)]
struct Options {
    base_roots: Vec<String>,
    new_roots: Vec<String>,
    cuts: Vec<String>,
    defers: Vec<String>,
    early_security_init: Vec<String>,
    slide: String,
}

/// `dma-prelink.json`: the V20 prelink this one inherits from.
#[derive(Deserialize)]
struct Previous {
    roots: Vec<String>,
    prelinked_sha256: String,
    report_sha256: String,
}

/// What this prelink reads of `dma-kernel.json`, the V20 prelink's report.
#[derive(Deserialize)]
struct InheritedReport {
    original_sha256: String,
    slide: i64,
    kernel_interfaces: Vec<String>,
    codeless_components: Vec<String>,
    components: Vec<String>,
    extents: Vec<Extent>,
    known_inline_data: Vec<InheritedInline>,
    original_executable_sha256: BTreeMap<String, String>,
}

#[derive(Deserialize)]
struct InheritedInline {
    va: u64,
    size: u64,
    sha256: String,
}

/// What iOS 17's prelink takes besides the two kernels.
pub struct Inputs17 {
    options: Options,
    previous: Previous,
    inherited_sha256: String,
    inherited: InheritedReport,
    sites: Vec<Site>,
}

impl Inputs17 {
    /// `prelink.json`, `dma-prelink.json`, `dma-kernel.json`, and the kind's declared sites.
    pub fn new(
        options: &[u8],
        previous: &[u8],
        inherited_report: &[u8],
        sites: Vec<Site>,
    ) -> std::result::Result<Self, Error> {
        Ok(Inputs17 {
            options: json("prelink.json", options)?,
            previous: json("dma-prelink.json", previous)?,
            inherited_sha256: hex::encode(Sha256::digest(inherited_report)),
            inherited: json("dma-kernel.json", inherited_report)?,
            sites,
        })
    }
}

/// The prelinked kernel: `kernel` (MH_EXECUTE, unprelinked) with the kexts of `kernelcache` (decompressed) the
/// inputs select.
pub fn prelink_17(kernel: &[u8], kernelcache: &[u8], inputs: &Inputs17) -> std::result::Result<Vec<u8>, Error> {
    let original = Original::checked(kernelcache, inputs)?;
    let cache = Kernelcache::read(kernelcache)?;
    let plan = plan::prepare(&cache, &original)?;
    native::apply(plan, kernel, &original.original_sha256)
}

pub(crate) struct Cut {
    sites: i128,
    stub: Option<&'static str>,
}

/// The checked inputs of the original kernelcache's half.
pub(crate) struct Original<'a> {
    roots: Vec<String>,
    new: BTreeSet<String>,
    cuts: BTreeMap<(String, String), Cut>,
    defers: HashMap<String, Vec<String>>,
    slide: i64,
    early_security_init: Vec<(String, i128)>,
    original_sha256: [u8; 32],
    inherited: &'a InheritedReport,
    sites: &'a [Site],
}

impl<'a> Original<'a> {
    fn checked(kernelcache: &[u8], inputs: &'a Inputs17) -> Result<Self> {
        let options = &inputs.options;
        let new: BTreeSet<String> = options.new_roots.iter().cloned().collect();
        let mut cuts = BTreeMap::new();
        for cut in &options.cuts {
            let [component, dependency, sites] = split(cut)?;
            let Some(sites) = pyint::parse(sites, 10) else {
                return refused(format!("Cut {cut}: not a pointer site count"));
            };
            let stub = (sites != 0).then_some(CUT_STUB);
            cuts.insert((component.to_owned(), dependency.to_owned()), Cut { sites, stub });
        }
        let mut defers: HashMap<String, Vec<String>> = HashMap::new();
        for defer in &options.defers {
            let [component, personality] = split(defer)?;
            defers
                .entry(component.to_owned())
                .or_default()
                .push(personality.to_owned());
        }
        let mut early_security_init: Vec<(String, i128)> = Vec::new();
        for request in &options.early_security_init {
            let (identifier, address) = request.split_once('=').unwrap_or((request, ""));
            let address = pyint::parse(address, 0);
            if !request.contains('=')
                || identifier.is_empty()
                || early_security_init.iter().any(|(i, _)| i == identifier)
            {
                return refused("Use one BUNDLE=ORIGINAL_VA initializer per module");
            }
            let Some(address) = address else {
                return refused(format!("Early security initializer {request}: not an address"));
            };
            early_security_init.push((identifier.to_owned(), address));
        }
        let Some(slide) = pyint::parse(&options.slide, 0).and_then(|s| i64::try_from(s).ok()) else {
            return refused(format!("Slide {}: not an integer", options.slide));
        };
        if inputs.inherited_sha256 != inputs.previous.report_sha256 {
            return refused("Only the completed V20 original-component report may be inherited");
        }
        let roots: Vec<String> = options.base_roots.iter().chain(&options.new_roots).cloned().collect();
        let previous_roots: Vec<&String> = inputs.previous.roots.iter().chain(&new).collect();
        if hex::encode(Sha256::digest(kernelcache)) != inputs.previous.prelinked_sha256
            || roots.iter().collect::<Vec<_>>() != previous_roots
        {
            return refused("Only the unchanged V20 roots are allowed");
        }
        let original_sha256 = &inputs.inherited.original_sha256;
        let Some(original_sha256) = hex_digest(original_sha256) else {
            return refused("A valid inherited original identity is required");
        };
        Ok(Original {
            roots,
            new,
            cuts,
            defers,
            slide,
            early_security_init,
            original_sha256,
            inherited: &inputs.inherited,
            sites: &inputs.sites,
        })
    }
}

fn split<const N: usize>(value: &str) -> Result<[&str; N]> {
    let parts: Vec<&str> = value.split('=').collect();
    parts
        .try_into()
        .or_else(|_| refused(format!("{value}: expected {N} fields separated by '='")))
}

fn hex_digest(text: &str) -> Option<[u8; 32]> {
    let lower = text.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    lower.then(|| hex::decode(text).ok()?.try_into().ok()).flatten()
}

/// `address + slide`, which must be a 64-bit address.
pub(crate) fn slid(address: impl Into<i128>, slide: i64) -> Result<u64> {
    let value = address.into() + i128::from(slide);
    u64::try_from(value).or_else(|_| refused(format!("Rebased address {value:#x} outside 64-bit address space")))
}
