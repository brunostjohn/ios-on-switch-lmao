// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;

use crate::{CrawlError, Result};

pub const MTLB_MAGIC: &[u8; 4] = b"MTLB";
pub const FAT_MAGIC: &[u8; 4] = &[0xCA, 0xFE, 0xBA, 0xBE];
pub const HEADER_LEN: usize = 0x58;
pub const AIR64_CPU_TYPE: u32 = 0x0100_0017;

pub const VERTEX: u8 = 0;
pub const FRAGMENT: u8 = 1;
pub const KERNEL: u8 = 2;
pub const EXTERN: u8 = 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AirSliceRule {
    pub kind: &'static str,
    pub first: bool,
    pub ceiling: u32,
    pub below_profile8: bool,
    pub air29: u32,
}

/// Mirrors `drivers/TegraGM20BMetal/plugin/MetallibSlice.h`'s `kAirSliceRules`.
pub fn rule_for_build(build: &str) -> Option<AirSliceRule> {
    match build {
        "21H461" => Some(AirSliceRule {
            kind: "21H461",
            first: true,
            ceiling: 0,
            below_profile8: false,
            air29: 0,
        }),
        "24B119" => Some(AirSliceRule {
            kind: "24B119",
            first: false,
            ceiling: 0x20009,
            below_profile8: true,
            air29: 0xD,
        }),
        _ => None,
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Constant {
    pub name: String,
    pub data_type: u8,
    pub index: u16,
    pub required: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Function {
    pub name: String,
    pub kind: u8,
    pub constants: Vec<Constant>,
    pub air: String,
    pub hash: Option<[u8; 32]>,
}

impl Function {
    pub fn required_constants(&self) -> Vec<&Constant> {
        self.constants.iter().filter(|c| c.required).collect()
    }
}

fn be_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn le_u16(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(data.get(at..at + 2)?.try_into().ok()?))
}

fn le_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn le_u64(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(data.get(at..at + 8)?.try_into().ok()?))
}

fn picks_slice(rule: AirSliceRule, current: Option<u32>, candidate: u32) -> bool {
    if candidate > rule.ceiling {
        return false;
    }
    if !rule.below_profile8 {
        return current.is_none_or(|cur| candidate > cur);
    }
    match current {
        None => candidate <= rule.air29,
        Some(cur) => candidate < rule.air29 && (cur >= rule.air29 || candidate > cur),
    }
}

pub fn slices(data: &[u8]) -> Vec<(u32, &[u8])> {
    if data.len() >= 8 && &data[..4] == FAT_MAGIC {
        let count = be_u32(data, 4).unwrap_or(0) as usize;
        let max_count = (data.len() - 8) / 20;
        let mut found = Vec::new();
        for i in 0..count.min(max_count) {
            let base = 8 + 20 * i;
            let subtype = be_u32(data, base + 4).unwrap_or(0);
            let offset = be_u32(data, base + 8).unwrap_or(0) as usize;
            let size = be_u32(data, base + 12).unwrap_or(0) as usize;
            if let Some(slice) = data.get(offset..offset.saturating_add(size))
                && slice.get(..4) == Some(MTLB_MAGIC.as_slice())
            {
                found.push((subtype, slice));
            }
        }
        return found;
    }
    if data.len() >= HEADER_LEN && &data[..4] == MTLB_MAGIC {
        return vec![(0, data)];
    }
    Vec::new()
}

pub fn thin(data: &[u8]) -> Option<&[u8]> {
    slices(data).into_iter().next().map(|(_, s)| s)
}

pub fn air_slice<'a>(data: &'a [u8], build: Option<&str>) -> Result<&'a [u8]> {
    if data.get(..4) != Some(FAT_MAGIC.as_slice()) {
        return Ok(data);
    }
    let rule = match build {
        Some(b) => Some(rule_for_build(b).ok_or_else(|| CrawlError::Invalid(format!("unknown IPSW build: {b}")))?),
        None => None,
    };
    let count = be_u32(data, 4).unwrap_or(0) as usize;
    let max_count = data.len().saturating_sub(8) / 20;
    let mut picked: Option<(usize, usize)> = None;
    let mut current: Option<u32> = None;
    for i in 0..count.min(max_count) {
        let base = 8 + 20 * i;
        let cpu = be_u32(data, base).unwrap_or(0);
        let sub = be_u32(data, base + 4).unwrap_or(0);
        let offset = be_u32(data, base + 8).unwrap_or(0) as usize;
        let size = be_u32(data, base + 12).unwrap_or(0) as usize;
        match rule {
            None => {
                if data.get(offset..offset.saturating_add(4)) == Some(MTLB_MAGIC.as_slice()) {
                    return Ok(data.get(offset..offset.saturating_add(size)).unwrap_or(&[]));
                }
            }
            Some(r) => {
                if cpu != AIR64_CPU_TYPE {
                    continue;
                }
                if r.first {
                    return Ok(data.get(offset..offset.saturating_add(size)).unwrap_or(&[]));
                }
                if picks_slice(r, current, sub) {
                    picked = Some((offset, size));
                    current = Some(sub);
                }
            }
        }
    }
    Ok(picked
        .and_then(|(off, sz)| data.get(off..off.saturating_add(sz)))
        .unwrap_or(&[]))
}

type TagMap<'a> = BTreeMap<[u8; 4], &'a [u8]>;

fn parse_tags(data: &[u8], mut at: usize, end: usize, first_wins: bool) -> Option<TagMap<'_>> {
    let mut found = BTreeMap::new();
    while at + 6 <= end {
        let tag: [u8; 4] = data.get(at..at + 4)?.try_into().ok()?;
        if &tag == b"ENDT" {
            break;
        }
        let length = usize::from(le_u16(data, at + 4)?);
        let value = data.get(at + 6..at + 6 + length)?;
        if first_wins {
            found.entry(tag).or_insert(value);
        } else {
            found.insert(tag, value);
        }
        at += 6 + length;
    }
    Some(found)
}

fn parse_constants(cnst: &[u8]) -> Option<Vec<Constant>> {
    let count = usize::from(le_u16(cnst, 0)?);
    let mut at = 2;
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        let nul_rel = cnst.get(at..)?.iter().position(|&b| b == 0)?;
        let end = at + nul_rel;
        let name = String::from_utf8_lossy(&cnst[at..end]).into_owned();
        let data_type = *cnst.get(end + 1)?;
        let index = le_u16(cnst, end + 2)?;
        let required = *cnst.get(end + 4)? != 0;
        out.push(Constant {
            name,
            data_type,
            index,
            required,
        });
        at = end + 5;
    }
    Some(out)
}

fn parse_versions(vers: Option<&[u8]>) -> String {
    let Some(v) = vers else {
        return String::new();
    };
    if v.len() < 8 {
        return String::new();
    }
    let air_major = u16::from_le_bytes([v[0], v[1]]);
    let air_minor = u16::from_le_bytes([v[2], v[3]]);
    let msl_major = u16::from_le_bytes([v[4], v[5]]);
    let msl_minor = u16::from_le_bytes([v[6], v[7]]);
    format!("air {air_major}.{air_minor} msl {msl_major}.{msl_minor}")
}

fn nul_name(raw: &[u8]) -> String {
    let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
    String::from_utf8_lossy(&raw[..end]).into_owned()
}

pub fn parse_functions_slice(mtlb: &[u8]) -> Option<Vec<Function>> {
    if mtlb.len() < HEADER_LEN || &mtlb[..4] != MTLB_MAGIC {
        return None;
    }
    let listing = usize::try_from(le_u64(mtlb, 0x18)?).ok()?;
    let public = usize::try_from(le_u64(mtlb, 0x28)?).ok()?;
    let count = usize::try_from(le_u32(mtlb, listing)?).ok()?;
    let mut at = listing + 4;
    let mut out = Vec::with_capacity(count.min(4096));
    for _ in 0..count {
        let size = usize::try_from(le_u32(mtlb, at)?).ok()?;
        if size < 4 || at.checked_add(size)? > mtlb.len() {
            return None;
        }
        let tags = parse_tags(mtlb, at + 4, at + size, false)?;
        at += size;
        let name_bytes = *tags.get(b"NAME")?;
        let type_bytes = *tags.get(b"TYPE")?;
        let kind = *type_bytes.first()?;
        let constants = if let Some(offt) = tags.get(b"OFFT") {
            let rel = usize::try_from(le_u64(offt, 0)?).ok()?;
            let start = public.checked_add(rel)?;
            let length = usize::try_from(le_u32(mtlb, start)?).ok()?;
            let pub_tags = parse_tags(mtlb, start + 4, start.checked_add(4)?.checked_add(length)?, false)?;
            match pub_tags.get(b"CNST") {
                Some(cnst) => parse_constants(cnst)?,
                None => Vec::new(),
            }
        } else {
            Vec::new()
        };
        let hash = tags.get(b"HASH").and_then(|h| <[u8; 32]>::try_from(*h).ok());
        out.push(Function {
            name: nul_name(name_bytes),
            kind,
            constants,
            air: parse_versions(tags.get(b"VERS").copied()),
            hash,
        });
    }
    Some(out)
}

pub fn parse_functions(data: &[u8]) -> Option<Vec<Function>> {
    parse_functions_slice(thin(data)?)
}

pub fn parse_functions_for_build(data: &[u8], build: Option<&str>) -> Result<Option<Vec<Function>>> {
    let slice = air_slice(data, build)?;
    if slice.is_empty() {
        return Ok(None);
    }
    Ok(parse_functions_slice(slice))
}

fn parse_entries_for_build<'a>(data: &'a [u8], build: Option<&str>) -> Result<Option<Vec<TagMap<'a>>>> {
    let slice = air_slice(data, build)?;
    if slice.len() < HEADER_LEN || &slice[..4] != MTLB_MAGIC {
        return Ok(None);
    }
    let Some(listing) = le_u64(slice, 24).and_then(|v| usize::try_from(v).ok()) else {
        return Ok(None);
    };
    let Some(count) = le_u32(slice, listing).and_then(|v| usize::try_from(v).ok()) else {
        return Ok(None);
    };
    let mut at = listing + 4;
    let mut found = Vec::with_capacity(count.min(4096));
    for _ in 0..count {
        let Some(size) = le_u32(slice, at).and_then(|v| usize::try_from(v).ok()) else {
            return Ok(None);
        };
        if size < 4 || at.checked_add(size).is_none_or(|end| end > slice.len()) {
            return Ok(None);
        }
        let Some(tags) = parse_tags(slice, at + 4, at + size, true) else {
            return Ok(None);
        };
        found.push(tags);
        at += size;
    }
    Ok(Some(found))
}

pub fn parse_names_for_build(data: &[u8], build: Option<&str>) -> Result<Option<Vec<String>>> {
    let Some(entries) = parse_entries_for_build(data, build)? else {
        return Ok(None);
    };
    Ok(Some(
        entries
            .into_iter()
            .filter_map(|e| e.get(b"NAME").map(|raw| nul_name(raw)))
            .collect(),
    ))
}

pub fn parse_function_hashes_for_build(data: &[u8], build: Option<&str>) -> Result<Option<BTreeMap<String, String>>> {
    let Some(entries) = parse_entries_for_build(data, build)? else {
        return Ok(None);
    };
    let mut out = BTreeMap::new();
    for entry in entries {
        if let (Some(name_raw), Some(hash_raw)) = (entry.get(b"NAME"), entry.get(b"HASH"))
            && hash_raw.len() == 32
        {
            out.insert(nul_name(name_raw), hex::encode(hash_raw));
        }
    }
    Ok(Some(out))
}
