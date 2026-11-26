// SPDX-License-Identifier: GPL-3.0-or-later
//! The prelinked kernel's `__PRELINK_INFO`: the selected entries slid, the new pointer list and the KCID.

use super::closure::{Closure, Kernelcache};
use super::coalesced::Coalesced;
use super::{Original, slid};
use crate::error::{Result, refused};
use crate::info::{self, Dict, Value};

const EARLY_SECURITY_INIT: &str = "_NativePrelinkEarlySecurityInit";
const SLID_KEYS: [&str; 3] = [
    "_PrelinkExecutableLoadAddr",
    "_PrelinkExecutableSourceAddr",
    "_PrelinkKmodInfo",
];

/// The kernel interfaces', components' and codeless components' entries, each as the prelinked kernel has it.
pub(crate) fn selected_entries(cache: &Kernelcache, closure: &Closure, original: &Original) -> Result<Vec<Value>> {
    let identifiers = closure
        .interfaces
        .iter()
        .chain(&closure.components)
        .chain(&closure.codeless);
    let mut selected = Vec::new();
    for identifier in identifiers {
        let mut entry = cache.entry(identifier)?.clone();
        if entry.contains(EARLY_SECURITY_INIT) {
            return refused("Refuse an initializer supplied by input metadata");
        }
        if let Some((_, address)) = original.early_security_init.iter().find(|(i, _)| i == identifier) {
            entry.set(
                EARLY_SECURITY_INIT,
                Value::Integer(address + i128::from(original.slide)),
            );
        }
        for key in SLID_KEYS {
            match entry.get_mut(key) {
                Some(Value::Integer(value)) if *value != 0 => *value += i128::from(original.slide),
                Some(value) if value.truthy() => return refused(format!("{identifier}: {key} is not an address")),
                _ => {}
            }
        }
        for (component, dependency) in original.cuts.keys() {
            if component == identifier && libraries(&mut entry, "OSBundleLibraries")?.remove(dependency).is_none() {
                return refused(format!("{identifier}: no OSBundleLibraries {dependency} to cut"));
            }
        }
        for name in original.defers.get(identifier).into_iter().flatten() {
            if libraries(&mut entry, "IOKitPersonalities")?.remove(name).is_none() {
                return refused(format!("{identifier}: no IOKitPersonalities {name} to defer"));
            }
        }
        selected.push(Value::Dict(entry));
    }
    Ok(selected)
}

fn libraries<'a>(entry: &'a mut Dict, key: &str) -> Result<&'a mut Dict> {
    entry
        .dict_mut(key)
        .map_or_else(|| refused(format!("Prelink info entry without a {key} dictionary")), Ok)
}

/// `_PrelinkLinkKASLROffsets`: the count, then each pointer site as an offset from the slid `__PRELINK_TEXT`.
pub(crate) fn kaslr_offsets(image: &Coalesced, closure: &Closure) -> Result<Vec<u8>> {
    let base = image.segment("__PRELINK_TEXT").va;
    let mut packed = Vec::with_capacity(4 + closure.bindings.len() * 4);
    packed.extend_from_slice(&(closure.bindings.len() as u32).to_le_bytes());
    for binding in &closure.bindings {
        let Ok(offset) = u32::try_from(i128::from(binding.new_site) - i128::from(base)) else {
            return refused("Rebased KASLR offset cannot fit original 32-bit format");
        };
        packed.extend_from_slice(&offset.to_le_bytes());
    }
    Ok(packed)
}

/// The prelink info XML with its terminating NUL.
pub(crate) fn prelink_info(entries: Vec<Value>, kaslr_offsets: Vec<u8>, kcid: &[u8]) -> Result<Vec<u8>> {
    let metadata = Dict(vec![
        ("_PrelinkInfoDictionary".to_owned(), Value::Array(entries)),
        ("_PrelinkLinkKASLROffsets".to_owned(), Value::Data(kaslr_offsets)),
        ("_PrelinkKCID".to_owned(), Value::Data(kcid.to_vec())),
    ]);
    let mut xml = info::serialize(&Value::Dict(metadata), &|value| match u64::try_from(value) {
        Ok(value) => Ok(value),
        Err(_) => refused("Metadata integer outside 64-bit range"),
    })?;
    xml.push(0);
    Ok(xml)
}

/// Each early initializer must be in its selected security extension's code, and a 64-bit address once slid.
pub(crate) fn check_early_security_init(cache: &Kernelcache, closure: &Closure, original: &Original) -> Result<()> {
    for (identifier, address) in &original.early_security_init {
        let module = closure.modules.iter().find(|(id, _)| id == identifier).map(|(_, m)| m);
        let extension = cache
            .get(identifier)
            .is_some_and(|e| e.is_true("AppleSecurityExtension"));
        let Some(module) = module.filter(|_| extension) else {
            return refused("Early initializer requires a selected security extension");
        };
        let in_code = module.segments.iter().any(|s| {
            s.name == "__TEXT_EXEC"
                && s.initprot == 5
                && s.maxprot & 2 == 0
                && i128::from(s.va) <= *address
                && address + 4 <= i128::from(s.va) + i128::from(s.filesize)
        });
        if address.rem_euclid(4) != 0 || !in_code {
            return refused("Early security initializer outside original module code");
        }
        if slid(*address, original.slide).is_err() {
            return refused("Early initializer address overflow");
        }
    }
    Ok(())
}
