// SPDX-License-Identifier: GPL-3.0-or-later
//! Step 3: every chained pointer in a prelinked kext written as the plain pointer it resolves to: a kext's own target
//! slid, Apple's kernel's through the bindings, a cut dependency's to our null stub.

use std::collections::{HashMap, HashSet};

use super::fixups::{self, chained_pointers};
use super::kernelcache::{Extents, KERNEL};
use super::{Prelink, Unresolved};
use crate::error::{Result, refused};
use crate::macho::put_u64;

/// `kmod_info.address`'s offset in XNU's `kmod_info`.
pub(crate) const KMOD_ADDRESS: u64 = 156;

pub(crate) struct Pointers {
    /// Each written site, slid, for `_PrelinkLinkKASLROffsets`: all but `kmod_info.address`, which OSKext slides.
    pub kaslr: Vec<u64>,
    /// Each written site, in Apple's addresses.
    pub written: HashSet<u64>,
}

pub(crate) fn bind(
    blobs: &mut [Vec<u8>; 4],
    p: &Prelink,
    cut_null: Option<u64>,
    unresolved: &mut Unresolved,
) -> Result<Pointers> {
    let mut kmod_addresses = HashSet::new();
    for id in &p.closure.selected {
        kmod_addresses.insert(u128::from(p.cache.kmod_info(id)?) + u128::from(KMOD_ADDRESS));
    }
    let extents = Extents::new(&p.cache);
    let mut cut_sites: HashMap<(&str, &str), i64> = HashMap::new();
    let mut pointers = Pointers {
        kaslr: Vec::new(),
        written: HashSet::new(),
    };
    for (site, raw) in chained_pointers(&p.cache)? {
        let Some(owner) = extents.owner(site).filter(|o| p.closure.selected.contains(*o)) else {
            continue;
        };
        let Some(target) = p.cache.base.checked_add(fixups::target(raw)) else {
            return refused(format!("{site:#x}: a pointer past the 64-bit address space"));
        };
        let value = match extents.owner(target) {
            Some(kext) if p.closure.selected.contains(kext) => p.layout.slid(target),
            Some(KERNEL) => p.bindings.resolve(target).unwrap_or_else(|| {
                if p.in_kernel_text(target) {
                    unresolved.function(owner, target);
                } else {
                    unresolved.data(owner, target);
                }
                0
            }),
            Some(dependency) if p.bindings.is_cut(owner, dependency) => {
                *cut_sites.entry((owner, dependency)).or_default() += 1;
                match cut_null {
                    Some(stub) => stub,
                    None => return refused("our kernel lacks _switch_prelink_cut_null"),
                }
            }
            other => {
                return refused(format!(
                    "pointer from {owner} to unselected {}",
                    other.unwrap_or("None")
                ));
            }
        };
        let Some((index, at)) = p.layout.at(site) else {
            return refused(format!("{site:#x}: a pointer outside the coalesced segments"));
        };
        put_u64(&mut blobs[index], at, value)?;
        pointers.written.insert(site);
        if !kmod_addresses.contains(&u128::from(site)) {
            pointers.kaslr.push(p.layout.slid(site));
        }
    }
    for cut in &p.bindings.cuts {
        let found = cut_sites
            .get(&(cut.component.as_str(), cut.dependency.as_str()))
            .copied()
            .unwrap_or(0);
        if found != cut.sites {
            return refused(format!(
                "cut {}={}: {found} pointer sites, declared {}",
                cut.component, cut.dependency, cut.sites
            ));
        }
    }
    Ok(pointers)
}
