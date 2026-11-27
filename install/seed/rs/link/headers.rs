// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::OnceLock;

use crate::records::fnv1a64;

const OWNED_H: &str = include_str!("../../../../drivers/TegraGM20BMetal/owned.h");
const CHIP_FACTS_H: &str = include_str!("../../../../drivers/TegraGM20BMetal/chip_facts.h");
const OWN_STATES_H: &str = include_str!("../../../../drivers/TegraGM20BMetal/own_states.h");
const SRGB_TABLE_H: &str = include_str!("../../../../drivers/TegraGM20BMetal/srgb_table.h");
pub const DEFAULT_DRAW_CONTEXT: &str = include_str!("../../../../drivers/TegraGM20BMetal/pack/draw_context.txt");

const SWIZZLE_METHOD: u32 = 0x0A18;
const SWIZZLE_VIEWPORTS: u32 = 16;

pub struct OwnedSpec {
    pub ranges: Vec<(u32, u32)>,
    pub vertex_streams: (u32, u32),
}

fn parse_u32_lit(s: &str) -> Option<u32> {
    let t = s.trim();
    if let Some(hex_part) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u32::from_str_radix(hex_part, 16).ok()
    } else {
        t.parse::<u32>().ok()
    }
}

fn parse_pair_braces(text: &str) -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'{'
            && let Some(close_rel) = text[i + 1..].find('}')
        {
            let inner = &text[i + 1..i + 1 + close_rel];
            if let Some((a, b)) = inner.split_once(',')
                && let (Some(va), Some(vb)) = (parse_u32_lit(a), parse_u32_lit(b))
            {
                out.push((va, vb));
            }
            i += close_rel + 2;
            continue;
        }
        i += 1;
    }
    out
}

pub fn owned_spec() -> &'static OwnedSpec {
    static CACHE: OnceLock<OwnedSpec> = OnceLock::new();
    CACHE.get_or_init(|| {
        let start = OWNED_H.find("kOwnedMethods[]").expect("kOwnedMethods[] in owned.h");
        let body_start = start + OWNED_H[start..].find('{').expect("'{' in kOwnedMethods") + 1;
        let end = body_start + OWNED_H[body_start..].find("};").expect("end of kOwnedMethods");
        let ranges = parse_pair_braces(&OWNED_H[body_start..end]);

        let vs_start = OWNED_H.find("kVertexStreams = ").expect("kVertexStreams in owned.h");
        let vs_line_end = vs_start + OWNED_H[vs_start..].find(';').expect("kVertexStreams ';'");
        let vs_pairs = parse_pair_braces(&OWNED_H[vs_start..vs_line_end]);
        let vertex_streams = vs_pairs[0];
        OwnedSpec { ranges, vertex_streams }
    })
}

pub fn owned_method(m: u32) -> bool {
    let spec = owned_spec();
    if m >= spec.vertex_streams.0 && m < spec.vertex_streams.1 && ((m & 0xF) == 4 || (m & 0xF) == 8) {
        return true;
    }
    spec.ranges.iter().any(|&(a, b)| m >= a && m < b)
}

pub fn dropped_methods() -> &'static BTreeSet<u32> {
    static CACHE: OnceLock<BTreeSet<u32>> = OnceLock::new();
    CACHE.get_or_init(|| {
        let spec = owned_spec();
        let mut dropped = BTreeSet::new();
        for &(a, b) in &spec.ranges {
            let mut m = a;
            while m < b {
                dropped.insert(m);
                m += 4;
            }
        }
        let mut m = spec.vertex_streams.0;
        while m < spec.vertex_streams.1 {
            if (m & 0xF) == 4 || (m & 0xF) == 8 {
                dropped.insert(m);
            }
            m += 4;
        }
        for j in 0..SWIZZLE_VIEWPORTS {
            dropped.insert(SWIZZLE_METHOD + 32 * j);
        }
        dropped
    })
}

pub fn chip_facts() -> u32 {
    static CACHE: OnceLock<u32> = OnceLock::new();
    *CACHE.get_or_init(|| {
        let pos = CHIP_FACTS_H
            .find("kProvenChipFacts = ")
            .expect("kProvenChipFacts in chip_facts.h");
        let rest = &CHIP_FACTS_H[pos + "kProvenChipFacts = ".len()..];
        let end = rest
            .find(|c: char| !(c.is_ascii_alphanumeric() || c == 'x' || c == 'X'))
            .unwrap_or(rest.len());
        parse_u32_lit(&rest[..end]).expect("valid kProvenChipFacts literal")
    })
}

pub fn own_states() -> &'static HashMap<String, u64> {
    static CACHE: OnceLock<HashMap<String, u64>> = OnceLock::new();
    CACHE.get_or_init(|| {
        let start = OWN_STATES_H.find("kOwnStates[]").expect("kOwnStates[] in own_states.h");
        let end = start + OWN_STATES_H[start..].find("};").expect("end of kOwnStates");
        let table = &OWN_STATES_H[start..end];
        let mut out = HashMap::new();
        let mut rest = table;
        while let Some(open) = rest.find('"') {
            let after_open = &rest[open + 1..];
            let Some(close) = after_open.find('"') else {
                break;
            };
            let pair = &after_open[..close];
            if let Some((vertex, fragment)) = pair.split_once(" | f ") {
                let family = vertex.rsplit_once('_').map_or("", |(head, _)| head);
                let name = format!("raw.{family}.{fragment}");
                out.insert(format!("{vertex} | f {fragment}"), fnv1a64(name.as_bytes()));
            }
            rest = &after_open[close + 1..];
        }
        out
    })
}

fn parse_c_array_u32(text: &str, name: &str) -> Vec<u32> {
    let start = text.find(name).expect("array name in srgb_table.h");
    let brace = start + text[start..].find('{').expect("'{' in array");
    let end = brace + text[brace..].find("};").expect("'};' in array");
    let body = &text[brace + 1..end];
    body.split(|c: char| !c.is_ascii_alphanumeric() && c != 'x' && c != 'X')
        .filter(|tok| !tok.is_empty())
        .filter_map(parse_u32_lit)
        .collect()
}

pub fn color_table_words() -> &'static [u32] {
    static CACHE: OnceLock<Vec<u32>> = OnceLock::new();
    CACHE.get_or_init(|| {
        let first = parse_c_array_u32(SRGB_TABLE_H, "kSrgbEncodeFirst[");
        let linear = parse_c_array_u32(SRGB_TABLE_H, "kSrgbLinear[");
        assert_eq!(first.len(), 256);
        assert_eq!(linear.len(), 256);
        let mut out = Vec::with_capacity(256 * 4);
        for b in 0..256 {
            out.push(first[b]);
            out.push(linear[b]);
            out.push(if b > 0 { linear[b - 1] } else { 0 });
            out.push(0);
        }
        out
    })
}

pub fn apply_draw_context(post: &BTreeMap<u32, u32>, draw_context_text: &str) -> BTreeMap<u32, u32> {
    let mut ctx = post.clone();
    for line in draw_context_text.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let mut parts = trimmed.split_ascii_whitespace();
        let (Some(method_s), Some(value_s)) = (parts.next(), parts.next()) else {
            continue;
        };
        if let Ok(method) = u32::from_str_radix(method_s, 16) {
            if value_s == "-" {
                ctx.remove(&method);
            } else if let Ok(val) = u32::from_str_radix(value_s, 16) {
                ctx.insert(method, val);
            }
        }
    }
    ctx
}
