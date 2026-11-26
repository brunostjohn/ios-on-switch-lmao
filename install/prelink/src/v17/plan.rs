// SPDX-License-Identifier: GPL-3.0-or-later
//! `emit_prelink.prepare`: everything the original kernelcache decides, with every byte written that does not
//! depend on the native kernel.

use std::collections::{BTreeSet, HashMap};

use sha2::{Digest, Sha256};

use super::closure::{Closure, Kernelcache, Target, plan_original};
use super::coalesced::{self, Coalesced};
use super::entries::{check_early_security_init, kaslr_offsets, selected_entries};
use super::inventory::{self, InlineData, pc_relative_target};
use super::{Original, slid};
use crate::error::{Result, refused};
use crate::info::Value;
use crate::sites;

pub(crate) struct CutSite {
    pub at: usize,
    pub component: String,
    pub dependency: String,
    pub stub: Option<&'static str>,
}

/// What `apply` needs from the original kernelcache.
pub(crate) struct Plan {
    pub image: Coalesced,
    /// Each kernel target once, with the original export names of it.
    pub targets: Vec<(u64, Vec<String>)>,
    /// Each kernel pointer's offset in `image` and index in `targets`.
    pub kernel_sites: Vec<(usize, usize)>,
    pub cut_sites: Vec<CutSite>,
    pub extents: Vec<(u64, u64)>,
    pub early_security_init: bool,
    /// The sparse header index: `(va, size, offset in image)`.
    pub index: (u64, u64, usize),
    pub entries: Vec<Value>,
    pub kaslr_offsets: Vec<u8>,
}

pub(crate) fn prepare(cache: &Kernelcache, original: &Original) -> Result<Plan> {
    let closure = plan_original(cache, original)?;
    check_inherited_contract(&closure, original)?;
    let mut inline_data: Vec<InlineData> = original
        .inherited
        .known_inline_data
        .iter()
        .map(|d| InlineData { va: d.va, size: d.size })
        .collect();
    let new_modules = closure
        .modules
        .iter()
        .filter(|(id, _)| original.new.contains(id))
        .map(|(_, m)| m);
    inline_data.extend(inventory::inline_data(new_modules, |va, size| {
        let at = cache.image.offset_of(va, size, cache.data.len())?;
        Ok(&cache.data[at..at + size as usize])
    })?);
    check_early_security_init(cache, &closure, original)?;
    let mut image = coalesced::copy_segments(cache, &closure, original.slide)?;
    write_sites(&mut image, &closure, original, &inline_data)?;
    let (targets, kernel_sites, cut_sites) = component_pointers(&mut image, &closure, original)?;
    coalesced::rebase_headers(&mut image, cache, &closure, original.slide)?;
    coalesced::rebase_kmod_info(&mut image, cache, &closure, original.slide)?;
    let index = coalesced::sparse_index(&mut image, cache, &closure, original.slide)?;
    // The native pointers are written last, which gives the same bytes only when no original-only write overlaps
    // one; none is in code either (checked with the copied code below).
    let native_sites = NativeSites::new(
        kernel_sites
            .iter()
            .map(|&(at, _)| at)
            .chain(cut_sites.iter().map(|c| c.at)),
    )?;
    if image.writes.iter().any(|&(at, length)| native_sites.hits(at, length)) {
        return refused("An original-only write overlaps a native kernel pointer site");
    }
    let entries = selected_entries(cache, &closure, original)?;
    let kaslr_offsets = kaslr_offsets(&image, &closure)?;
    check_executable_bytes(&image, cache, &closure, &native_sites, original)?;
    Ok(Plan {
        image,
        targets,
        kernel_sites,
        cut_sites,
        extents: closure.extents.iter().map(|e| (e.va, e.size)).collect(),
        early_security_init: !original.early_security_init.is_empty(),
        index,
        entries,
        kaslr_offsets,
    })
}

/// Reuse only the V20 findings wholly determined by the unchanged original modules and the fixed slide.
fn check_inherited_contract(closure: &Closure, original: &Original) -> Result<()> {
    let inherited = original.inherited;
    if original.slide != inherited.slide {
        return refused("Inherited original contract changed: slide");
    }
    if closure.interfaces != inherited.kernel_interfaces {
        return refused("Inherited original contract changed: kernel_interfaces");
    }
    let codeless: BTreeSet<&String> = closure.codeless.iter().collect();
    let new_codeless: BTreeSet<&String> = original.new.iter().filter(|n| codeless.contains(n)).collect();
    let expected: BTreeSet<&String> = inherited
        .codeless_components
        .iter()
        .chain(new_codeless.iter().copied())
        .collect();
    if codeless != expected {
        return refused("Inherited original contract changed: codeless_components");
    }
    let components: BTreeSet<&String> = closure.components.iter().collect();
    let new_coded = original.new.iter().filter(|n| !new_codeless.contains(n));
    let expected: BTreeSet<&String> = inherited.components.iter().chain(new_coded).collect();
    if components != expected {
        return refused("Original component closure differs from V20");
    }
    let old_extents: Vec<_> = closure
        .extents
        .iter()
        .filter(|e| !original.new.contains(&e.module))
        .collect();
    if old_extents != inherited.extents.iter().collect::<Vec<_>>() {
        return refused("An inherited original component extent changed");
    }
    Ok(())
}

/// The kind's declared sites in the copied code. A site is neither a PC-relative encoding, before or after, nor in
/// known inline data: the inherited findings describe Apple's code.
fn write_sites(image: &mut Coalesced, closure: &Closure, original: &Original, inline: &[InlineData]) -> Result<()> {
    for site in original.sites {
        let Some((_, module)) = closure.modules.iter().find(|(id, _)| *id == site.bundle) else {
            return refused(format!(
                "{}: sites declared for a kext that is not prelinked",
                site.bundle
            ));
        };
        if !module
            .segments
            .iter()
            .any(|s| s.name == "__TEXT_EXEC" && s.holds(site.at, 4))
        {
            return refused(format!(
                "{} {:#x}: a declared site outside its code",
                site.bundle, site.at
            ));
        }
        if [site.original, site.replacement]
            .iter()
            .any(|&w| pc_relative_target(w, site.at).is_some())
        {
            return refused(format!(
                "{} {:#x}: a declared site with a PC-relative encoding",
                site.bundle, site.at
            ));
        }
        if inline
            .iter()
            .any(|d| d.va <= site.at && u128::from(site.at) < u128::from(d.va) + u128::from(d.size))
        {
            return refused(format!(
                "{} {:#x}: a declared site in known inline data",
                site.bundle, site.at
            ));
        }
    }
    let slide = original.slide;
    let at = |image: &Coalesced, address: u64| image.new_offset(slid(address, slide)?, 4);
    sites::apply(
        original.sites,
        image,
        |image, address| image.u32_at(at(image, address)?),
        |image, address, word| {
            let offset = at(image, address)?;
            image.write(offset, &word.to_le_bytes());
            Ok(())
        },
    )
}

type Pointers = (Vec<(u64, Vec<String>)>, Vec<(usize, usize)>, Vec<CutSite>);

/// The component pointers written; the kernel and cut pointers' offsets kept for `apply`.
fn component_pointers(image: &mut Coalesced, closure: &Closure, original: &Original) -> Result<Pointers> {
    let (mut targets, mut groups, mut kernel_sites, mut cut_sites) =
        (Vec::new(), HashMap::new(), Vec::new(), Vec::new());
    for binding in &closure.bindings {
        let at = image.new_offset(binding.new_site, 8)?;
        match &binding.target {
            Target::Kernel { target, names } => {
                let group = *groups.entry(*target).or_insert_with(|| {
                    targets.push((*target, names.clone()));
                    targets.len() - 1
                });
                kernel_sites.push((at, group));
            }
            Target::Cut { component, dependency } => cut_sites.push(CutSite {
                at,
                component: component.clone(),
                dependency: dependency.clone(),
                stub: original.cuts[&(component.clone(), dependency.clone())].stub,
            }),
            Target::Component(target) => image.write(at, &target.to_le_bytes()),
        }
    }
    Ok((targets, kernel_sites, cut_sites))
}

pub(crate) struct NativeSites(Vec<usize>);

impl NativeSites {
    fn new(sites: impl Iterator<Item = usize>) -> Result<Self> {
        let mut sites: Vec<usize> = sites.collect();
        sites.sort_unstable();
        if sites.windows(2).any(|pair| pair[1] - pair[0] < 8) {
            return refused("Native kernel pointer sites overlap");
        }
        Ok(NativeSites(sites))
    }

    fn hits(&self, at: usize, length: usize) -> bool {
        let index = self.0.partition_point(|&site| site < at.saturating_sub(7));
        index < self.0.len() && self.0[index] < at + length
    }
}

/// Every executable byte is Apple's except at the kind's declared sites. A kext with sites that the V20 prelink
/// already took has its Apple hash checked against the inherited one, since its prelinked code no longer has it.
fn check_executable_bytes(
    image: &Coalesced,
    cache: &Kernelcache,
    closure: &Closure,
    native_sites: &NativeSites,
    original: &Original,
) -> Result<()> {
    let inherited = &original.inherited.original_executable_sha256;
    let mut hashes: HashMap<&str, Option<String>> = HashMap::new();
    for (identifier, module) in &closure.modules {
        for segment in module
            .segments
            .iter()
            .filter(|s| s.name == "__TEXT_EXEC" && s.filesize != 0)
        {
            let old = cache.image.offset_of(segment.va, segment.filesize, cache.data.len())?;
            let new = image.new_offset(slid(segment.va, original.slide)?, segment.filesize)?;
            let length = segment.filesize as usize;
            let apple = &cache.data[old..old + length];
            let mut expected = apple.to_vec();
            let own: Vec<_> = original.sites.iter().filter(|s| segment.holds(s.at, 1)).collect();
            for site in &own {
                let at = (site.at - segment.va) as usize;
                let Some(word) = expected.get_mut(at..at + 4) else {
                    return refused("A declared site past its code");
                };
                word.copy_from_slice(&site.replacement.to_le_bytes());
            }
            if image.output[new..new + length] != expected[..] || native_sites.hits(new, length) {
                return refused("Original executable bytes changed outside the declared sites");
            }
            let hash = match (original.new.contains(identifier), own.is_empty()) {
                (true, _) => None,
                (false, false) => Some(hex::encode(Sha256::digest(apple))),
                (false, true) => match inherited.get(identifier) {
                    Some(hash) => Some(hash.clone()),
                    None => return refused(format!("{identifier}: no inherited executable hash")),
                },
            };
            hashes.insert(identifier, hash);
        }
    }
    let old: HashMap<&str, &String> = hashes.iter().filter_map(|(id, h)| Some((*id, h.as_ref()?))).collect();
    let inherited_map: HashMap<&str, &String> = inherited.iter().map(|(id, h)| (id.as_str(), h)).collect();
    let keys: BTreeSet<&str> = hashes.keys().copied().collect();
    let expected_keys: BTreeSet<&str> = inherited
        .keys()
        .map(String::as_str)
        .chain(
            closure
                .modules
                .iter()
                .map(|(id, _)| id.as_str())
                .filter(|id| original.new.contains(*id)),
        )
        .collect();
    if old != inherited_map || keys != expected_keys {
        return refused("Original executable identity set changed");
    }
    Ok(())
}
