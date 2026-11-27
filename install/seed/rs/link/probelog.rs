// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs;
use std::path::Path;

use crate::headers::owned_method;
use crate::packfile::{key_hash_str, variant_name};
use crate::records::RawStateRecord;
use crate::{LinkError, Result};

pub(crate) const PROGRAM_OFFSETS: [u32; 2] = [0x2044, 0x2144];
const SUBC_COPY: u16 = 4;
const V24_AT: usize = 704;

pub(crate) struct ProbeView {
    pub(crate) index: usize,
    pub(crate) targets: u32,
    pub(crate) regs: HashMap<u32, u32>,
    pub(crate) vs_pid: String,
    pub(crate) fs_pid: Option<String>,
}

fn targets_mask(key: &str) -> u32 {
    let mut mask = 0u32;
    for tok in key.split_ascii_whitespace() {
        let bytes = tok.as_bytes();
        if bytes.len() >= 3 && bytes[0] == b'c' && bytes[1].is_ascii_digit() && tok.contains('=') {
            mask |= 1u32 << (bytes[1] - b'0');
        }
    }
    mask
}

fn target_of_method(m: u32) -> Option<u32> {
    if (0x1E00..0x1F00).contains(&m) {
        Some((m - 0x1E00) >> 5)
    } else if (0x1A00..0x1A20).contains(&m) {
        Some((m - 0x1A00) >> 2)
    } else if (0x1360..0x1380).contains(&m) {
        Some((m - 0x1360) >> 2)
    } else {
        None
    }
}

pub(crate) fn is_foreign(m: u32, mask: u32) -> bool {
    target_of_method(m).is_some_and(|t| ((mask >> t) & 1) == 0)
}

fn is_disabled_stream(m: u32, v: u32) -> bool {
    (0x1C00..0x1E00).contains(&m) && (m - 0x1C00).is_multiple_of(0x10) && (v & 0x1000) == 0
}

pub(crate) type InitTrace = (Vec<(u16, u16, u32)>, BTreeMap<u32, u32>);

pub(crate) fn load_init(path: &Path) -> Result<InitTrace> {
    let text = fs::read_to_string(path)?;
    let mut init = Vec::new();
    let mut post0 = BTreeMap::new();
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("T ") {
            let mut parts = rest.split_ascii_whitespace();
            if let (Some(sc_s), Some(m_s), Some(d_s)) = (parts.next(), parts.next(), parts.next())
                && let (Ok(sc), Ok(m), Ok(d)) = (
                    sc_s.parse::<u16>(),
                    u16::from_str_radix(m_s, 16),
                    u32::from_str_radix(d_s, 16),
                )
                && sc != SUBC_COPY
            {
                init.push((sc, m, d));
            }
        } else if let Some(rest) = line.strip_prefix("S regs ") {
            let mut parts = rest.split_ascii_whitespace();
            if parts.next() == Some("0") {
                for tok in parts {
                    if let Some((a_s, v_s)) = tok.split_once('=')
                        && let (Ok(a), Ok(v)) = (u32::from_str_radix(a_s, 16), u32::from_str_radix(v_s, 16))
                    {
                        post0.insert(a, v);
                    }
                }
            }
        }
    }
    Ok((init, post0))
}

pub(crate) fn load_init_records(path: &Path) -> Result<Vec<(u16, u16, u32, u32)>> {
    let text = fs::read_to_string(path)?;
    let mut out = Vec::new();
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("I ") {
            let mut parts = rest.split_ascii_whitespace();
            if let (Some(sc_s), Some(m_s), Some(d_s), Some(r_s)) =
                (parts.next(), parts.next(), parts.next(), parts.next())
            {
                let sc = sc_s
                    .parse::<u16>()
                    .map_err(|e| LinkError::Invalid(format!("context.log I subc: {e}")))?;
                let m = u16::from_str_radix(m_s, 16)
                    .map_err(|e| LinkError::Invalid(format!("context.log I method: {e}")))?;
                let d =
                    u32::from_str_radix(d_s, 16).map_err(|e| LinkError::Invalid(format!("context.log I data: {e}")))?;
                let reloc = r_s
                    .parse::<u32>()
                    .map_err(|e| LinkError::Invalid(format!("context.log I reloc: {e}")))?;
                out.push((sc, m, d, reloc));
            }
        }
    }
    Ok(out)
}

pub(crate) fn split_identities(states: &mut [RawStateRecord]) {
    let mut variants: HashMap<String, HashSet<Vec<u8>>> = HashMap::new();
    let mut identities: Vec<(String, Vec<u8>)> = Vec::with_capacity(states.len());
    for rec in states.iter() {
        let whole = rec.programs[1].program_id();
        let identity = if let Some(split) = &rec.split {
            let mut id = split.record.clone();
            for p in &split.programs {
                id.extend_from_slice(&p.program_id_u64().to_be_bytes());
            }
            id
        } else {
            Vec::new()
        };
        variants.entry(whole.clone()).or_default().insert(identity.clone());
        identities.push((whole, identity));
    }
    for (rec, (whole, identity)) in states.iter_mut().zip(identities) {
        if variants[&whole].len() > 1 {
            rec.programs[1].identity = identity;
            rec.programs[1].storage_id = Some(whole);
        }
    }
}

pub(crate) fn make_probes(states: &[RawStateRecord], context: &BTreeMap<u32, u32>) -> Vec<ProbeView> {
    let mut context_cache: HashMap<u32, HashMap<u32, u32>> = HashMap::new();
    let mut out = Vec::with_capacity(states.len());
    for (index, rec) in states.iter().enumerate() {
        let mask = targets_mask(&rec.key);
        let base = context_cache.entry(mask).or_insert_with(|| {
            context
                .iter()
                .filter(|&(&m, _)| !is_foreign(m, mask))
                .map(|(&m, &v)| (m, if is_disabled_stream(m, v) { 0 } else { v }))
                .collect()
        });
        let mut regs = base.clone();
        for (&m, &v) in &rec.regs {
            if !is_foreign(m, mask) {
                regs.insert(m, if is_disabled_stream(m, v) { 0 } else { v });
            }
        }
        let vs_pid = rec.programs[0].program_id();
        let fs_pid = (!rec.programs[1].image.is_empty()).then(|| rec.programs[1].program_id());
        if !rec.programs[0].image.is_empty() {
            regs.insert(
                PROGRAM_OFFSETS[0],
                (rec.programs[0].program_id_u64() & 0xFFFF_FFFF) as u32,
            );
        }
        if !rec.programs[1].image.is_empty() {
            regs.insert(
                PROGRAM_OFFSETS[1],
                (rec.programs[1].program_id_u64() & 0xFFFF_FFFF) as u32,
            );
        }
        out.push(ProbeView {
            index,
            targets: mask,
            regs,
            vs_pid,
            fs_pid,
        });
    }
    out
}

pub(crate) fn split_registers(probes: &[ProbeView], post0: &BTreeMap<u32, u32>) -> (Vec<u32>, BTreeMap<u32, u32>) {
    let mut all_regs: BTreeSet<u32> = post0.keys().copied().collect();
    for p in probes {
        all_regs.extend(p.regs.keys().copied());
    }
    let mut varying = Vec::new();
    let mut baseline = BTreeMap::new();
    for m in all_regs {
        if owned_method(m) {
            continue;
        }
        let target = target_of_method(m);
        let mut first: Option<u32> = None;
        let mut varies = false;
        match target {
            None => {
                for p in probes {
                    let val = p.regs.get(&m).copied().unwrap_or(0);
                    match first {
                        None => first = Some(val),
                        Some(f) if f != val => {
                            varies = true;
                            break;
                        }
                        _ => {}
                    }
                }
            }
            Some(t) => {
                let bit = 1u32 << t;
                for p in probes {
                    if (p.targets & bit) == 0 {
                        continue;
                    }
                    let val = p.regs.get(&m).copied().unwrap_or(0);
                    match first {
                        None => first = Some(val),
                        Some(f) if f != val => {
                            varies = true;
                            break;
                        }
                        _ => {}
                    }
                }
            }
        }
        if varies {
            varying.push(m);
        } else if let Some(val) = first
            && val != post0.get(&m).copied().unwrap_or(0)
        {
            baseline.insert(m, val);
        }
    }
    (varying, baseline)
}

pub(crate) fn set_tiers(states: &mut [RawStateRecord], tiers: &HashMap<u64, u32>) -> Result<Vec<u64>> {
    let hashes: Vec<u64> = states
        .iter()
        .map(|rec| key_hash_str(&rec.key, variant_name(&rec.key)))
        .collect();
    for (rec, &h) in states.iter_mut().zip(&hashes) {
        rec.tier = tiers.get(&h).copied().unwrap_or(0);
    }
    let mut twins: HashMap<u64, u32> = HashMap::new();
    for rec in states.iter() {
        let slice = rec
            .pipeline
            .get(V24_AT + 8..V24_AT + 16)
            .ok_or_else(|| LinkError::Invalid(format!("pipeline record truncated ({} bytes)", rec.pipeline.len())))?;
        let twin = u64::from_le_bytes(slice.try_into().expect("8"));
        if twin != 0 {
            twins
                .entry(twin)
                .and_modify(|cur| *cur = (*cur).min(rec.tier))
                .or_insert(rec.tier);
        }
    }
    for (rec, &h) in states.iter_mut().zip(&hashes) {
        if let Some(&t) = twins.get(&h) {
            rec.tier = rec.tier.min(t);
        }
    }
    Ok(hashes)
}

pub(crate) fn variant_report(states: &[RawStateRecord]) -> Vec<String> {
    const FORMS: [(&str, &str); 4] = [
        ("tests=", "of a test class"),
        ("cubic", "of cubic samplers"),
        ("xrs=", "of sampled two-plane textures"),
        ("wtex=", "of written formats"),
    ];
    let named: Vec<&str> = states.iter().map(|r| variant_name(&r.key)).collect();
    let counts = FORMS
        .iter()
        .map(|&(tag, what)| format!("{} {what}", named.iter().filter(|n| n.contains(tag)).count()))
        .collect::<Vec<_>>()
        .join(", ");
    let mut out = vec![format!(
        "{} variant records: {counts}",
        named.iter().filter(|n| !n.is_empty()).count()
    )];
    let mut seen = HashSet::new();
    for (rec, &vn) in states.iter().zip(&named) {
        if vn.contains("xrs=") && !vn.contains("tests=") && seen.insert((vn, rec.key.as_str())) {
            let short: String = rec.key.chars().take(150).collect();
            out.push(format!(
                "  sampled two-plane variant ({}): {short}",
                vn.strip_prefix(" | ").unwrap_or(vn)
            ));
        }
    }
    out
}
