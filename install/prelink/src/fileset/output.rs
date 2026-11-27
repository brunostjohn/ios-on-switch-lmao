// SPDX-License-Identifier: GPL-3.0-or-later
//! Steps 6 and 7: the coalesced segments appended to our kernel, the sparse header index at the end of
//! `__PRELINK_TEXT`, and the prelink info with the selected entries slid, the KASLR offsets and the KCID.

use sha2::{Digest, Sha256};

use super::Prelink;
use super::layout::{COALESCED, PAGE};
use crate::error::{Result, refused};
use crate::info::{self, Dict, Value};
use crate::macho::{Image, NativeImage, ORDINARY, put_u64};

const KCID_DOMAIN: &[u8] = b"SWITCH-26.5.2-A64X-KCID\0";
/// Each coalesced segment's protection, in `COALESCED` order.
const PROTECTION: [u32; 4] = [1, 1, 5, 3];
const SLID_KEYS: [&str; 3] = [
    "_PrelinkExecutableLoadAddr",
    "_PrelinkExecutableSourceAddr",
    "_PrelinkKmodInfo",
];

pub(crate) fn emit(
    kernel: &[u8],
    native: &Image,
    mut blobs: [Vec<u8>; 4],
    p: &Prelink,
    kaslr: &[u64],
    kernelcache_sha256: &[u8],
) -> Result<Vec<u8>> {
    let mut image = NativeImage::new(native, kernel, PAGE);
    let mut prelink_text = (0, 0, 0);
    for (index, (name, _)) in COALESCED.into_iter().enumerate() {
        let blob = &mut blobs[index];
        blob.resize(blob.len().next_multiple_of(PAGE as usize), 0);
        let va = p.layout.slid(p.layout.ranges[index].0);
        let fileoff = image.append_segment(name, va, blob, blob.len() as u64, PROTECTION[index])?;
        if index == 0 {
            prelink_text = (va, blob.len() as u64, fileoff);
        }
    }
    let (index_va, index_size, index_at) = sparse_index(&mut image.output, p, prelink_text)?;
    let kcid = Sha256::new()
        .chain_update(KCID_DOMAIN)
        .chain_update(kernel)
        .chain_update(kernelcache_sha256)
        .finalize();
    let metadata = Dict(vec![
        ("_PrelinkInfoDictionary".to_owned(), Value::Array(entries(p)?)),
        (
            "_PrelinkLinkKASLROffsets".to_owned(),
            Value::Data(kaslr_offsets(kaslr, prelink_text.0)?),
        ),
        ("_PrelinkKCID".to_owned(), Value::Data(kcid[..16].to_vec())),
    ]);
    let mut xml = info::serialize(&Value::Dict(metadata), &|value| Ok(value as u64))?;
    xml.push(0);
    xml.resize(xml.len().next_multiple_of(PAGE as usize), 0);
    let top = native
        .segments
        .iter()
        .filter(|s| s.size != 0)
        .map(|s| u128::from(s.va) + u128::from(s.size))
        .max();
    let Some(Ok(info_va)) = top.map(|top| u64::try_from(top.next_multiple_of(u128::from(PAGE)))) else {
        return refused("No address above our kernel's segments for the prelink info");
    };
    image.append_segment("__PRELINK_INFO", info_va, &xml, xml.len() as u64, 3)?;
    image.add_sparse_index_section(index_va, index_size, index_at)?;
    Image::parse(&image.output, 0, Some(ORDINARY))?;
    Ok(image.output)
}

/// Every selected kext's slid header address, sorted, in the last bytes of `__PRELINK_TEXT` past the kexts' headers:
/// `(va, size, file offset)`.
fn sparse_index(
    output: &mut [u8],
    p: &Prelink,
    (text_va, text_size, text_fileoff): (u64, u64, usize),
) -> Result<(u64, u64, usize)> {
    let mut headers = Vec::with_capacity(p.closure.selected.len());
    let mut occupied = None;
    for id in &p.closure.selected {
        headers.push(i128::from(p.cache.load_address(id)?) + i128::from(p.layout.slide));
        let text = p.cache.entry(id)?.image.segments.iter().filter(|s| s.name == "__TEXT");
        occupied = text
            .map(|s| i128::from(s.va) + i128::from(s.size))
            .chain(occupied)
            .max();
    }
    headers.sort_unstable();
    let Some(occupied) = occupied else {
        return refused("No prelinked kext has a __TEXT segment");
    };
    let size = 8 * headers.len() as u64;
    let va = (i128::from(text_va) + i128::from(text_size) - i128::from(size)) & !7;
    let occupied = i128::from((occupied + i128::from(p.layout.slide)) as u64);
    if va < occupied {
        return refused("no room for the sparse index in __PRELINK_TEXT");
    }
    let va = va as u64;
    let at = text_fileoff + (va - text_va) as usize;
    for (k, header) in headers.into_iter().enumerate() {
        put_u64(output, at + 8 * k, header as u64)?;
    }
    Ok((va, size, at))
}

/// The kernel interfaces' entries without load addresses, then the selected and codeless kexts', the selected ones'
/// addresses slid, with the cut dependencies and deferred personalities taken out.
fn entries(p: &Prelink) -> Result<Vec<Value>> {
    let mut entries = Vec::new();
    for id in &p.closure.interfaces {
        let mut entry = p.cache.info(id)?.clone();
        entry.remove(SLID_KEYS[0]);
        entry.remove(SLID_KEYS[1]);
        entries.push(Value::Dict(entry));
    }
    for id in p.closure.selected.iter().chain(&p.closure.codeless) {
        let mut entry = p.cache.info(id)?.clone();
        if p.closure.selected.contains(id) {
            for key in SLID_KEYS {
                match entry.get_mut(key) {
                    Some(Value::Integer(value)) if *value != 0 => {
                        *value = i128::from(p.layout.slid(*value as u64));
                    }
                    Some(value) if value.truthy() => return refused(format!("{id}: {key} is not an address")),
                    _ => {}
                }
            }
        }
        for cut in p.bindings.cuts.iter().filter(|c| c.component == *id) {
            match entry.get_mut("OSBundleLibraries") {
                Some(Value::Dict(libraries)) => {
                    libraries.remove(&cut.dependency);
                }
                None => {}
                Some(_) => return refused(format!("{id}: OSBundleLibraries is not a dictionary")),
            }
        }
        for personality in p.bindings.deferred.get(id).into_iter().flatten() {
            let personalities = entry.dict_mut("IOKitPersonalities");
            if personalities.and_then(|d| d.remove(personality)).is_none() {
                return refused(format!("{id}: no IOKitPersonalities {personality} to defer"));
            }
        }
        entries.push(Value::Dict(entry));
    }
    Ok(entries)
}

/// `_PrelinkLinkKASLROffsets`: the count, then each pointer site's offset from the slid `__PRELINK_TEXT`, sorted.
fn kaslr_offsets(sites: &[u64], base: u64) -> Result<Vec<u8>> {
    let mut offsets: Vec<i128> = sites.iter().map(|&s| i128::from(s) - i128::from(base)).collect();
    offsets.sort_unstable();
    let fits = |o: &i128| u32::try_from(*o).is_ok();
    if !offsets.first().is_none_or(fits) || !offsets.last().is_none_or(fits) {
        return refused("KASLR offset outside 32 bits");
    }
    let mut packed = Vec::with_capacity(4 * (offsets.len() + 1));
    packed.extend_from_slice(&(offsets.len() as u32).to_le_bytes());
    for offset in offsets {
        packed.extend_from_slice(&(offset as u32).to_le_bytes());
    }
    Ok(packed)
}
