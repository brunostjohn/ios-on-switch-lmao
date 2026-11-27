// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet, HashSet};

use crate::records::{RawStateRecord, fnv1a64};

const HELD_KERNELS: u64 = 0;

fn strip_linked_libs(line: &str) -> String {
    let mut out = String::with_capacity(line.len());
    let mut rest = line;
    while let Some(pos) = rest.find(" | ") {
        out.push_str(&rest[..pos + 3]);
        rest = &rest[pos + 3..];
        let is_linked = matches!(rest.get(..3), Some("vl " | "vp " | "fl " | "fp "));
        if is_linked {
            let tag = &rest[..3];
            let after_tag = &rest[3..];
            let fn_end = after_tag
                .find(|c: char| c.is_ascii_whitespace())
                .unwrap_or(after_tag.len());
            out.push_str(tag);
            out.push_str(&after_tag[..fn_end]);
            rest = &after_tag[fn_end..];
            if let Some(after_lib) = rest.strip_prefix(" lib=") {
                let lib_end = after_lib
                    .find(|c: char| c.is_ascii_whitespace())
                    .unwrap_or(after_lib.len());
                rest = &after_lib[lib_end..];
            }
        }
    }
    out.push_str(rest);
    out
}

fn find_libs(line: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut rest = line;
    while let Some(pos) = rest.find(" lib=") {
        let after = &rest[pos + 5..];
        let end = after.find(|c: char| c.is_ascii_whitespace()).unwrap_or(after.len());
        if end > 0 {
            out.push(&after[..end]);
        }
        rest = &after[end..];
    }
    out
}

fn strip_all_libs(line: &str) -> String {
    let mut out = String::with_capacity(line.len());
    let mut rest = line;
    while let Some(pos) = rest.find(" lib=") {
        out.push_str(&rest[..pos]);
        let after = &rest[pos + 5..];
        let end = after.find(|c: char| c.is_ascii_whitespace()).unwrap_or(after.len());
        rest = &after[end..];
    }
    out.push_str(rest);
    out
}

fn strip_stage_names(stage_part: &str) -> String {
    let mut out = String::with_capacity(stage_part.len());
    let mut rest = stage_part;
    while let Some(pos) = rest.find(" | ") {
        out.push_str(&rest[..pos + 3]);
        rest = &rest[pos + 3..];
        if matches!(rest.get(..2), Some("v " | "f ")) {
            out.push_str(&rest[..2]);
            rest = &rest[2..];
            let fn_end = rest.find(|c: char| c.is_ascii_whitespace()).unwrap_or(rest.len());
            out.push_str(&rest[..fn_end]);
            rest = &rest[fn_end..];
            if let Some(after_as) = rest.strip_prefix(" as=") {
                let end = after_as
                    .find(|c: char| c.is_ascii_whitespace())
                    .unwrap_or(after_as.len());
                if end > 0 {
                    rest = &after_as[end..];
                }
            }
            if let Some(after_spec) = rest.strip_prefix(" spec")
                && after_spec.starts_with(' ')
            {
                rest = after_spec;
            }
        }
    }
    out.push_str(rest);
    out
}

/// Mirrors `drivers/TegraGM20BMetal/pack/make_pack.py:state_key`.
pub fn state_key(raw_line: &str) -> String {
    let without_linked = strip_linked_libs(raw_line);
    let libs = find_libs(&without_linked);
    let mut framework = "";
    if let Some(&first_lib) = libs.first()
        && first_lib.ends_with(".framework/default.metallib")
        && !first_lib.contains("/QuartzCore.framework/")
        && let Some((prefix, _)) = first_lib.rsplit_once(".framework/")
    {
        framework = prefix.rsplit_once('/').map_or(prefix, |(_, tail)| tail);
    }
    let line = strip_all_libs(raw_line);
    let (Some(bar1), Some(bar2)) = (line.find(" | v "), line.find(" | f ")) else {
        return String::new();
    };
    let bar3 = line[bar2 + 5..].find(" | ").map(|rel| bar2 + 5 + rel);
    let head_tokens: Vec<&str> = line[..bar1].split_ascii_whitespace().collect();
    let mut fields: Vec<&str> = head_tokens
        .iter()
        .copied()
        .filter(|h| {
            h.starts_with("samples=")
                || h.starts_with("topo=")
                || h.starts_with("ds=")
                || h.starts_with("alphacov=")
                || (h.len() > 1 && matches!(h.as_bytes()[0], b'c' | b'a') && h.as_bytes()[1].is_ascii_digit())
        })
        .collect();
    for &h in &head_tokens {
        if h == "rast=0" {
            fields.push(h);
        }
    }
    let stage_slice = &line[bar1..bar3.unwrap_or(line.len())];
    let mut key = format!("{} {}", fields.join(" "), strip_stage_names(stage_slice));
    let mut cubic = String::new();
    let mut sampled = String::new();
    let mut written = String::new();
    if let Some(b3) = bar3 {
        for segment in line[b3 + 3..].split(" | ") {
            if matches!(segment.get(..3), Some("vl " | "vp " | "fl " | "fp ")) {
                let tokens: Vec<&str> = segment.split_ascii_whitespace().collect();
                let mut head_part: Vec<&str> = tokens.iter().take(2).copied().collect();
                let mut values: Vec<&str> = Vec::new();
                for &t in tokens.iter().skip(2) {
                    if t.starts_with("as=") {
                        head_part.push(t);
                    } else if t != "spec" {
                        values.push(t);
                    }
                }
                key.push_str(" | ");
                key.push_str(&head_part.join(" "));
                key.push(' ');
                key.push_str(&values.join(" "));
            } else if segment.starts_with("cubic ") {
                let collapsed = segment.split_ascii_whitespace().collect::<Vec<_>>().join(" ");
                cubic = format!(" | {collapsed}");
            } else if segment.starts_with("xrs=") {
                sampled = format!(" | {}", segment.trim());
            } else if segment.starts_with("wtex=") {
                written = format!(" | {}", segment.trim());
            }
        }
    }
    let combined = if framework.is_empty() {
        key
    } else {
        format!("{key} | l {framework}")
    };
    format!("{}{cubic}{sampled}{written}", combined.trim())
}

pub fn compute_key(line: &str) -> String {
    let Some(k_pos) = line.find(" | k ") else {
        return String::new();
    };
    let after_k = &line[k_pos + 5..];
    let Some(next_bar) = after_k.find(" |") else {
        return String::new();
    };
    let k_seg = &after_k[..next_bar];
    let fn_end = k_seg.find(|c: char| c.is_ascii_whitespace()).unwrap_or(k_seg.len());
    if fn_end == 0 {
        return String::new();
    }
    let func_name = &k_seg[..fn_end];
    let mut rest = &k_seg[fn_end..];
    if let Some(after_lib) = rest.strip_prefix(" lib=") {
        let lib_end = after_lib
            .find(|c: char| c.is_ascii_whitespace())
            .unwrap_or(after_lib.len());
        if lib_end > 0 {
            rest = &after_lib[lib_end..];
        }
    }
    let head = line.split_once(" | ").map_or(line, |(h, _)| h);
    let mut max_val = "0";
    let mut tgm_val = "0";
    for tok in head.split_ascii_whitespace() {
        if let Some((k, v)) = tok.split_once('=') {
            if k == "max" {
                max_val = v;
            } else if k == "tgm" {
                tgm_val = v;
            }
        }
    }
    let values: Vec<&str> = rest.split_ascii_whitespace().filter(|&t| t != "spec").collect();
    if values.is_empty() {
        format!("k {func_name} | max={max_val} tgm={tgm_val}")
    } else {
        format!("k {func_name} {} | max={max_val} tgm={tgm_val}", values.join(" "))
    }
}

pub fn refused_keys(states: &[RawStateRecord], refused: &[RawStateRecord]) -> HashSet<u64> {
    let made: HashSet<u64> = states.iter().map(|r| fnv1a64(r.key.as_bytes())).collect();
    refused
        .iter()
        .map(|r| fnv1a64(r.key.as_bytes()))
        .filter(|h| !made.contains(h))
        .collect()
}

pub struct DeclaredArchives {
    pub by_path: BTreeMap<String, BTreeSet<u64>>,
    pub render_rows: usize,
    pub compute_rows: usize,
}

pub fn declared(inventory: &str, refused: &HashSet<u64>) -> DeclaredArchives {
    let mut by_path: BTreeMap<String, BTreeSet<u64>> = BTreeMap::new();
    let mut render_rows = 0usize;
    let mut compute_rows = 0usize;
    for row in inventory.lines() {
        let mut cols = row.split('\t');
        let (Some(kind), Some(_owner), Some(path), Some(family), Some(line)) =
            (cols.next(), cols.next(), cols.next(), cols.next(), cols.next())
        else {
            continue;
        };
        if family != "device" {
            continue;
        }
        let key = match kind {
            "render" => state_key(line),
            "compute" => compute_key(line),
            _ => String::new(),
        };
        if key.is_empty() {
            continue;
        }
        let hash = fnv1a64(key.as_bytes());
        if kind == "render" && refused.contains(&hash) {
            continue;
        }
        by_path.entry(path.to_owned()).or_default().insert(hash);
        if kind == "render" {
            render_rows += 1;
        } else if kind == "compute" {
            compute_rows += 1;
        }
    }
    DeclaredArchives {
        by_path,
        render_rows,
        compute_rows,
    }
}

pub fn held_kernels(kernel_keys: &[String]) -> BTreeSet<u64> {
    kernel_keys
        .iter()
        .map(|key| {
            let state = key.split_once(" | tg=").map_or(key.as_str(), |(a, _)| a);
            fnv1a64(state.as_bytes())
        })
        .collect()
}

pub fn section_bytes(declared: &DeclaredArchives, held: &BTreeSet<u64>) -> Vec<u8> {
    let mut lists: Vec<(u64, Vec<u64>)> = declared
        .by_path
        .iter()
        .map(|(path, keys)| (fnv1a64(path.as_bytes()), keys.iter().copied().collect()))
        .collect();
    lists.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| a.1.cmp(&b.1)));
    lists.insert(0, (HELD_KERNELS, held.iter().copied().collect()));

    let mut heads = Vec::with_capacity(lists.len() * 16);
    let mut keys: Vec<u64> = Vec::new();
    for (path_hash, hashes) in &lists {
        heads.extend_from_slice(&path_hash.to_le_bytes());
        heads.extend_from_slice(&(keys.len() as u32).to_le_bytes());
        heads.extend_from_slice(&(hashes.len() as u32).to_le_bytes());
        keys.extend_from_slice(hashes);
    }
    let mut out = Vec::with_capacity(8 + heads.len() + keys.len() * 8);
    out.extend_from_slice(&(lists.len() as u32).to_le_bytes());
    out.extend_from_slice(&(keys.len() as u32).to_le_bytes());
    out.extend_from_slice(&heads);
    for k in keys {
        out.extend_from_slice(&k.to_le_bytes());
    }
    out
}

pub fn report_line(declared: &DeclaredArchives, held: &BTreeSet<u64>) -> String {
    let keys: usize = declared.by_path.values().map(BTreeSet::len).sum();
    format!(
        "archive key lists: {} archives, {keys} keys ({} render rows, {} compute rows); {} compute states held as kernels",
        declared.by_path.len(),
        declared.render_rows,
        declared.compute_rows,
        held.len()
    )
}
