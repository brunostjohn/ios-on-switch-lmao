// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

use crate::metallib::parse_names_for_build;
use crate::{CrawlError, Result};

pub const QC_PATH: &str = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
const GUESSES: usize = 3;
const INSTALLED_APP_PREFIX: &str = "/private/var/containers/Bundle/Application/";
const STAGED_APPS_PREFIX: &str = "/private/var/staged_system_apps/";

fn path_parts(device: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    if device.starts_with('/') {
        parts.push("/");
    }
    for seg in device.split('/').filter(|s| !s.is_empty()) {
        parts.push(seg);
    }
    parts
}

fn collect_relative_files(root: &Path, dir: &Path, out: &mut Vec<String>) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_relative_files(root, &path, out)?;
        } else if path.is_file() {
            let rel = path
                .strip_prefix(root)
                .map_err(|e| CrawlError::Invalid(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            out.push(rel);
        }
    }
    Ok(())
}

pub fn index_names(build: &str, root: &Path, devices: Option<&[String]>) -> Result<BTreeMap<String, Vec<String>>> {
    let discovered: Vec<String>;
    let raw_devices: &[String] = match devices {
        Some(list) if !list.is_empty() => list,
        _ => {
            let mut found = Vec::new();
            collect_relative_files(root, root, &mut found)?;
            discovered = found;
            &discovered
        }
    };
    let mut normalized: Vec<String> = raw_devices
        .iter()
        .map(|d| format!("/{}", d.trim_start_matches('/')))
        .collect();
    normalized.sort_by(|a, b| path_parts(a).cmp(&path_parts(b)));

    let mut table: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for device in normalized {
        let rel = device.trim_start_matches('/');
        let data = fs::read(root.join(rel))?;
        let found = parse_names_for_build(&data, Some(build))?
            .ok_or_else(|| CrawlError::Invalid(format!("{}/{device} is not a Metal library", root.display())))?;
        let unique: BTreeSet<String> = found.into_iter().collect();
        for name in unique {
            table.entry(name).or_default().push(device.clone());
        }
    }
    Ok(table)
}

/// Formats `names.json` identically to Python's `json.dumps(dict(sorted(table.items())), indent=0) + '\n'`.
pub fn format_names_json(table: &BTreeMap<String, Vec<String>>) -> Result<String> {
    if table.is_empty() {
        return Ok("{}\n".to_owned());
    }
    let mut out = String::from("{\n");
    for (idx, (name, paths)) in table.iter().enumerate() {
        if idx > 0 {
            out.push_str(",\n");
        }
        out.push_str(&serde_json::to_string(name)?);
        out.push_str(": ");
        if paths.is_empty() {
            out.push_str("[]");
        } else {
            out.push_str("[\n");
            for (pidx, path) in paths.iter().enumerate() {
                if pidx > 0 {
                    out.push_str(",\n");
                }
                out.push_str(&serde_json::to_string(path)?);
            }
            out.push_str("\n]");
        }
    }
    out.push_str("\n}\n");
    Ok(out)
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

fn url_unquote(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%'
            && i + 2 < bytes.len()
            && let (Some(hi), Some(lo)) = (hex_val(bytes[i + 1]), hex_val(bytes[i + 2]))
        {
            out.push((hi << 4) | lo);
            i += 3;
            continue;
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn url_quote_path(input: &str) -> String {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut out = String::with_capacity(input.len());
    for &b in input.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'_' | b'~' | b'/' | b':') {
            out.push(b as char);
        } else {
            out.push('%');
            out.push(HEX[(b >> 4) as usize] as char);
            out.push(HEX[(b & 0xF) as usize] as char);
        }
    }
    out
}

fn match_installed_app(path: &str) -> Option<&str> {
    let rest = path.strip_prefix(INSTALLED_APP_PREFIX)?;
    if rest.len() < 37 || rest.as_bytes()[36] != b'/' {
        return None;
    }
    let uuid = &rest.as_bytes()[..36];
    for (idx, &b) in uuid.iter().enumerate() {
        if matches!(idx, 8 | 13 | 18 | 23) {
            if b != b'-' {
                return None;
            }
        } else if !b.is_ascii_hexdigit() {
            return None;
        }
    }
    let after_uuid = &rest[37..];
    let slash = after_uuid.find('/')?;
    if slash > 4 && after_uuid[..slash].ends_with(".app") && slash + 1 < after_uuid.len() {
        Some(after_uuid)
    } else {
        None
    }
}

pub fn library_path(library: &str, available: &BTreeSet<String>) -> String {
    let mut path = url_unquote(library);
    if let Some(stripped) = path.strip_prefix("/System/Cryptexes/OS/") {
        path = format!("/{stripped}");
    }
    if path.starts_with("/var/") {
        path = format!("/private{path}");
    }
    if available.contains(&path) {
        return url_quote_path(&path);
    }
    if let Some(app_rel) = match_installed_app(&path) {
        let staged = format!("{STAGED_APPS_PREFIX}{app_rel}");
        if available.contains(&staged) {
            return url_quote_path(&staged);
        }
    }
    library.to_owned()
}

pub fn relocate_line(line: &str, available: &BTreeSet<String>) -> String {
    let bytes = line.as_bytes();
    let mut out = String::with_capacity(line.len());
    let mut i = 0;
    while i < bytes.len() {
        if i > 0 && bytes[i - 1] == b' ' && line[i..].starts_with("lib=") {
            let val_start = i + 4;
            let mut val_end = val_start;
            while val_end < bytes.len() && !bytes[val_end].is_ascii_whitespace() {
                val_end += 1;
            }
            if val_end > val_start {
                out.push_str("lib=");
                out.push_str(&library_path(&line[val_start..val_end], available));
                i = val_end;
                continue;
            }
        }
        let ch = line[i..].chars().next().expect("non-empty slice");
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

#[derive(Clone, Debug)]
struct StageHit {
    insert_at: usize,
    lib: Option<String>,
    candidates: Option<Vec<Option<String>>>,
}

fn is_stage_tag(tag: &str) -> bool {
    matches!(tag, "vl" | "vp" | "fl" | "fp" | "v" | "f" | "k" | "l" | "p")
}

fn parse_s_stage(line: &str, candidates: &dyn Fn(&str) -> Vec<Option<String>>) -> Option<StageHit> {
    let rest = line.strip_prefix("S ok=1 ")?;
    let mut pos = "S ok=1 ".len();
    let mut lib = None;
    let after_lib = if let Some(lib_rest) = rest.strip_prefix("lib=") {
        let sp = lib_rest.find(' ')?;
        if sp == 0 {
            return None;
        }
        lib = Some(lib_rest[..sp].to_owned());
        pos += 4 + sp + 1;
        &lib_rest[sp + 1..]
    } else {
        rest
    };
    let name_rest = after_lib.strip_prefix("name=")?;
    let sp = name_rest.find(' ')?;
    if sp == 0 {
        return None;
    }
    let name = &name_rest[..sp];
    let cands = if lib.is_some() { None } else { Some(candidates(name)) };
    Some(StageHit {
        insert_at: pos,
        lib,
        candidates: cands,
    })
}

fn parse_pipe_stages(line: &str, candidates: &dyn Fn(&str) -> Vec<Option<String>>) -> Vec<StageHit> {
    let mut stages = Vec::new();
    let bytes = line.as_bytes();
    let mut i = 0;
    while i + 2 <= bytes.len() {
        if bytes[i] == b'|' && bytes[i + 1] == b' ' {
            let tag_start = i + 2;
            let Some(next_bar_rel) = line[tag_start..].find(" |") else {
                break;
            };
            let seg_end = tag_start + next_bar_rel;
            let seg = &line[tag_start..seg_end];
            if let Some(sp1) = seg.find(' ') {
                let tag = &seg[..sp1];
                if is_stage_tag(tag) {
                    let after_tag = &seg[sp1 + 1..];
                    let name_len = after_tag
                        .find(|c: char| c.is_ascii_whitespace())
                        .unwrap_or(after_tag.len());
                    let name = &after_tag[..name_len];
                    if !name.is_empty() && name != "-" {
                        let rest = &after_tag[name_len..];
                        let lib = rest
                            .split_ascii_whitespace()
                            .find_map(|tok| tok.strip_prefix("lib="))
                            .map(str::to_owned);
                        let cands = if lib.is_some() { None } else { Some(candidates(name)) };
                        stages.push(StageHit {
                            insert_at: tag_start + sp1 + 1 + name_len,
                            lib,
                            candidates: cands,
                        });
                    }
                }
            }
            i = seg_end + 1;
            continue;
        }
        i += 1;
    }
    stages
}

fn choose_libraries(stages: &[StageHit]) -> (&'static str, Vec<Option<String>>) {
    let open: Vec<&Vec<Option<String>>> = stages
        .iter()
        .filter(|s| s.lib.is_none())
        .filter_map(|s| s.candidates.as_ref())
        .collect();
    if open.is_empty() {
        return (if stages.is_empty() { "unknown" } else { "journaled" }, vec![None]);
    }
    let mut common: BTreeSet<Option<String>> = open[0].iter().cloned().collect();
    for list in &open[1..] {
        let next_set: BTreeSet<Option<String>> = list.iter().cloned().collect();
        common.retain(|item| next_set.contains(item));
    }
    let any_open = open.iter().any(|l| !l.is_empty());
    let how = if common.len() == 1 && common.contains(&None) {
        "qc"
    } else if common.len() == 1 {
        "resolved"
    } else if common.len() > 1 && common.len() <= GUESSES {
        "guessed"
    } else if any_open {
        "ambiguous"
    } else {
        "unknown"
    };
    if matches!(how, "qc" | "resolved" | "guessed") {
        let mut choices: Vec<Option<String>> = common.into_iter().collect();
        choices.sort_by(|a, b| a.as_deref().unwrap_or("").cmp(b.as_deref().unwrap_or("")));
        (how, choices)
    } else {
        (how, vec![None])
    }
}

fn apply_library(line: &str, is_s: bool, stages: &[StageHit], choice: Option<&str>) -> String {
    let Some(choice) = choice else {
        return line.to_owned();
    };
    let mut text = line.to_owned();
    for stage in stages.iter().rev() {
        if stage.lib.is_some() {
            continue;
        }
        if is_s {
            text.insert_str(stage.insert_at, &format!("lib={choice} "));
        } else {
            text.insert_str(stage.insert_at, &format!(" lib={choice}"));
        }
    }
    text
}

#[derive(Clone, Debug, Default)]
pub struct ResolveResult {
    pub text: String,
    pub rows: BTreeMap<String, BTreeMap<(char, String), usize>>,
}

pub fn resolve_corpus(names_table: &BTreeMap<String, Vec<String>>, corpus_text: &str) -> ResolveResult {
    let available: BTreeSet<String> = names_table.values().flat_map(|v| v.iter().cloned()).collect();
    let qc_names: BTreeSet<&str> = names_table
        .iter()
        .filter(|(_, paths)| paths.iter().any(|p| p == QC_PATH))
        .map(|(name, _)| name.as_str())
        .collect();

    let candidates = |name: &str| -> Vec<Option<String>> {
        if qc_names.contains(name) {
            vec![None]
        } else {
            names_table
                .get(name)
                .map(|paths| paths.iter().cloned().map(Some).collect())
                .unwrap_or_default()
        }
    };

    let mut out_lines = Vec::new();
    let mut rows: BTreeMap<String, BTreeMap<(char, String), usize>> = BTreeMap::new();
    for original in corpus_text.lines() {
        let kind_char = original.chars().next();
        let Some(kind) = kind_char.filter(|c| matches!(c, 'S' | 'R' | 'C' | 'D')) else {
            out_lines.push(original.to_owned());
            continue;
        };
        let line = relocate_line(original, &available);
        let stages = if kind == 'S' {
            parse_s_stage(&line, &candidates).into_iter().collect()
        } else {
            parse_pipe_stages(&line, &candidates)
        };
        let (how, choices) = choose_libraries(&stages);
        for choice in choices {
            out_lines.push(apply_library(&line, kind == 'S', &stages, choice.as_deref()));
            let first = stages
                .iter()
                .find_map(|s| s.lib.as_deref())
                .or(choice.as_deref())
                .unwrap_or(QC_PATH);
            let row_key = if matches!(how, "qc" | "journaled" | "resolved" | "guessed") {
                first.to_owned()
            } else {
                how.to_owned()
            };
            *rows
                .entry(row_key)
                .or_default()
                .entry((kind, how.to_owned()))
                .or_insert(0) += 1;
        }
    }
    let mut text = out_lines.join("\n");
    text.push('\n');
    ResolveResult { text, rows }
}
