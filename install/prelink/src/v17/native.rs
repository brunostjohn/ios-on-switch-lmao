// SPDX-License-Identifier: GPL-3.0-or-later
//! `emit_prelink.apply`: the native kernel with its exports bound, the coalesced segments and the prelink info
//! appended, the KCID, and the sparse index's section in the header.

use std::collections::{BTreeSet, HashMap};

use sha2::{Digest, Sha256};

use super::coalesced::VM_PAGE;
use super::entries::prelink_info;
use super::plan::Plan;
use crate::error::{Result, refused};
use crate::macho::{self, Image, MH_EXECUTE, NativeImage, ORDINARY};

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
    let mut image = NativeImage::new(&native, native_data, FILE_PAGE);
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
    let (index_va, index_size, index_at) = plan.index;
    image.add_sparse_index_section(index_va, index_size, base + index_at)?;
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
