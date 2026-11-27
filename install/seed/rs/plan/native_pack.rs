// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use crate::restamp::{CACHE_ENTRY_FORMAT, SLOT_PAGE_BYTES, parse_stack_hex};
use crate::seedpack::{DEFAULT_BUCKET_BYTES, EntrySource, IDENTITY_BYTES, SeedPackEntry, write_seed_pack};
use crate::{PlanError, Result};

const FACTS_ONLY_PREFIX: &str = "D ok=1 tg=0,0,0 edge=0,0,0 threads=0 ";

pub fn native_lines(lines: &[String]) -> Vec<String> {
    lines
        .iter()
        .filter(|line| {
            (line.starts_with("R ") || line.starts_with("D "))
                && line.contains(" ok=1 ")
                && !line.starts_with(FACTS_ONLY_PREFIX)
        })
        .cloned()
        .collect()
}

pub fn split_native_lines(text: &str) -> (Vec<String>, Vec<String>) {
    let marker = "# derived\n";
    if let Some((obs, der)) = text.split_once(marker) {
        let obs_lines: Vec<String> = obs.lines().map(str::to_owned).collect();
        let der_lines: Vec<String> = der.lines().map(str::to_owned).collect();
        (native_lines(&obs_lines), native_lines(&der_lines))
    } else {
        let obs_lines: Vec<String> = text.lines().map(str::to_owned).collect();
        (native_lines(&obs_lines), Vec::new())
    }
}

fn is_lower_hex16(text: &str) -> bool {
    text.len() == 16 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn check_canonical_seed_rel(stack: &str, name: &str, ext: &str, got: &str) -> Result<String> {
    if !is_lower_hex16(stack) || !is_lower_hex16(name) {
        return Err(PlanError::Invalid(format!(
            "invalid seed artifact identity stack={stack:?} name={name:?}"
        )));
    }
    let expected = format!("rec-{stack}/{name}.{ext}");
    if got != expected {
        return Err(PlanError::Invalid(format!(
            "manifest artifact path {got:?} does not match canonical {expected:?}"
        )));
    }
    Ok(expected)
}

fn read_regular_file(path: &Path) -> Result<Vec<u8>> {
    let meta = fs::symlink_metadata(path)?;
    if meta.file_type().is_symlink() || !meta.is_file() {
        return Err(PlanError::Invalid(format!(
            "seed artifact is not a regular file: {}",
            path.display()
        )));
    }
    Ok(fs::read(path)?)
}

pub fn pack_lists(manifest: &Value) -> Result<BTreeMap<String, (u32, String)>> {
    let stack = manifest
        .get("stack")
        .and_then(Value::as_str)
        .ok_or_else(|| PlanError::Invalid("manifest missing stack".into()))?;
    let stack_u64 = parse_stack_hex(stack)?;
    let stack_canon = format!("{stack_u64:016x}");
    let records = manifest
        .get("records")
        .and_then(Value::as_array)
        .ok_or_else(|| PlanError::Invalid("manifest missing records".into()))?;
    let mut out: BTreeMap<String, (u32, String)> = BTreeMap::new();
    for row in records {
        if row.get("status").and_then(Value::as_str) != Some("made") {
            continue;
        }
        let tier = u32::from(row.get("derived").and_then(Value::as_bool).unwrap_or(false));
        let name = row
            .get("name")
            .and_then(Value::as_str)
            .ok_or_else(|| PlanError::Invalid("made row missing name".into()))?;
        let artifact = row
            .get("artifact")
            .and_then(Value::as_str)
            .ok_or_else(|| PlanError::Invalid("made row missing artifact".into()))?;
        let canon_art = check_canonical_seed_rel(&stack_canon, name, "rec", artifact)?;
        let mut pairs = vec![(name, canon_art)];
        if let Some(slots) = row.get("slots").and_then(Value::as_array) {
            for slot in slots {
                let sname = slot
                    .get("name")
                    .and_then(Value::as_str)
                    .ok_or_else(|| PlanError::Invalid("slot missing name".into()))?;
                let spath = slot
                    .get("path")
                    .and_then(Value::as_str)
                    .ok_or_else(|| PlanError::Invalid("slot missing path".into()))?;
                let canon_slot = check_canonical_seed_rel(&stack_canon, sname, "slot", spath)?;
                pairs.push((sname, canon_slot));
            }
        }
        for (entry_name, entry_path) in pairs {
            if let Some((prev_tier, prev_path)) = out.get_mut(entry_name) {
                if prev_path != &entry_path {
                    return Err(PlanError::Invalid(format!(
                        "native seed has two files named {entry_name}"
                    )));
                }
                *prev_tier = (*prev_tier).min(tier);
            } else {
                out.insert(entry_name.to_owned(), (tier, entry_path));
            }
        }
    }
    Ok(out)
}

pub fn pack_native_seed(seed_dir: &Path, out_dir: &Path) -> Result<PathBuf> {
    let manifest: Value = serde_json::from_str(&fs::read_to_string(seed_dir.join("manifest.json"))?)?;
    let stack = manifest
        .get("stack")
        .and_then(Value::as_str)
        .ok_or_else(|| PlanError::Invalid("manifest missing stack".into()))?;
    let stack_u64 = parse_stack_hex(stack)?;
    let stack_canon = format!("{stack_u64:016x}");
    let records = pack_lists(&manifest)?;
    let mut entries = Vec::with_capacity(records.len());
    for (name_hex, (tier, rel_path)) in records {
        let name = hex::decode(&name_hex).map_err(|_| PlanError::Invalid(format!("invalid entry hex {name_hex}")))?;
        let full = seed_dir.join(&rel_path);
        let meta = fs::symlink_metadata(&full)?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err(PlanError::Invalid(format!(
                "seed artifact is not a regular file: {}",
                full.display()
            )));
        }
        entries.push(SeedPackEntry {
            name,
            tier,
            source: EntrySource::Path(full),
        });
    }
    let mut identity = [0u8; IDENTITY_BYTES];
    identity[..8].copy_from_slice(&stack_u64.to_le_bytes());
    let target = out_dir.join(format!("rec-{stack_canon}"));
    write_seed_pack(&target, 8, &identity, entries, DEFAULT_BUCKET_BYTES)?;
    Ok(target)
}

pub fn build_coverage(
    manifest: &Value,
    expected_observed: usize,
    root: &Path,
    ipsw: &str,
    stack: &str,
) -> Result<Value> {
    let stack_u64 = parse_stack_hex(stack)?;
    let stack_canon = format!("{stack_u64:016x}");
    if manifest.get("stack").and_then(Value::as_str) != Some(stack_canon.as_str())
        || manifest.get("schema").and_then(Value::as_u64) != Some(1)
    {
        return Err(PlanError::Invalid("native seed and compiler identity differ".into()));
    }
    let records = manifest
        .get("records")
        .and_then(Value::as_array)
        .ok_or_else(|| PlanError::Invalid("manifest missing records".into()))?;
    let mut artifacts: BTreeMap<String, Value> = BTreeMap::new();
    let mut observed_made = 0usize;
    let mut seen_ops: BTreeSet<(String, i64, String, bool)> = BTreeSet::new();
    for row in records {
        let derived = row.get("derived").and_then(Value::as_bool).unwrap_or(false);
        let status = row.get("status").and_then(Value::as_str).unwrap_or("");
        if let (Some(src), Some(line), Some(inp)) = (
            row.get("source").and_then(Value::as_str),
            row.get("line").and_then(Value::as_i64),
            row.get("input").and_then(Value::as_str),
        ) && seen_ops.insert((src.to_owned(), line, inp.to_owned(), derived))
            && !derived
            && status == "made"
        {
            observed_made += 1;
        }
        if status == "made" {
            let name = row
                .get("name")
                .and_then(Value::as_str)
                .ok_or_else(|| PlanError::Invalid("made row missing name".into()))?;
            let rel = row
                .get("artifact")
                .and_then(Value::as_str)
                .ok_or_else(|| PlanError::Invalid("made row missing artifact".into()))?;
            let canon_rel = check_canonical_seed_rel(&stack_canon, name, "rec", rel)?;
            if let std::collections::btree_map::Entry::Vacant(e) = artifacts.entry(canon_rel) {
                let data = read_regular_file(&root.join(e.key()))?;
                e.insert(json!({"bytes": data.len(), "sha256": hex::encode(Sha256::digest(&data))}));
            }
            if let Some(slots) = row.get("slots").and_then(Value::as_array) {
                for slot in slots {
                    let sname = slot
                        .get("name")
                        .and_then(Value::as_str)
                        .ok_or_else(|| PlanError::Invalid("slot missing name".into()))?;
                    let srel = slot
                        .get("path")
                        .and_then(Value::as_str)
                        .ok_or_else(|| PlanError::Invalid("slot missing path".into()))?;
                    let canon_slot = check_canonical_seed_rel(&stack_canon, sname, "slot", srel)?;
                    if let std::collections::btree_map::Entry::Vacant(e) = artifacts.entry(canon_slot) {
                        let data = read_regular_file(&root.join(e.key()))?;
                        if !data.len().is_multiple_of(SLOT_PAGE_BYTES) {
                            return Err(PlanError::Invalid(format!("slot not page-aligned: {}", e.key())));
                        }
                        e.insert(json!({"bytes": data.len(), "sha256": hex::encode(Sha256::digest(&data))}));
                    }
                }
            }
        }
    }
    Ok(json!({
        "artifacts": artifacts,
        "cacheVersion": manifest.get("cacheVersion").cloned().unwrap_or(json!(CACHE_ENTRY_FORMAT)),
        "complete": expected_observed > 0 && observed_made == expected_observed,
        "expected_observed": expected_observed,
        "ipsw": ipsw,
        "packVersion": manifest.get("packVersion").cloned().unwrap_or(json!(37)),
        "schema": 1,
        "scope": "resolved-native-render-and-dispatch-records",
        "stack": stack_canon,
    }))
}
