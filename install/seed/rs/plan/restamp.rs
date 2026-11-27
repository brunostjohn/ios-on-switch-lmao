// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value, json};

use crate::{PlanError, Result};

pub const CACHE_ENTRY_MAGIC: &[u8; 8] = b"SWRAWREC";
pub const CACHE_ENTRY_FORMAT: u32 = 20;
const KIND_RECORD: u32 = 1;
const KIND_KERNEL: u32 = 3;
const PACK_PIPELINE_BYTES: usize = 736;
const PACK_SPLIT_BYTES: usize = 322;
const CONTROL_WORD_OFFSET: usize = 636;
const PROGRAM_PREFETCH_BYTES: usize = 0x1000;
pub const SLOT_PAGE_BYTES: usize = 16384;
const SLOT_DOMAIN: u64 = 0x736c_6f74_0000_0000;
const MAX_ENTRY_BYTES: usize = 1 << 24;

pub fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in bytes {
        h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    h
}

pub fn cache_key_name(stack: u64, vertex_air: &[u8; 32], fragment_air: &[u8; 32], key: &str) -> u64 {
    let h = fnv1a64(&stack.to_le_bytes());
    let mut buf = Vec::with_capacity(8 + 32 + 32 + key.len());
    buf.extend_from_slice(&h.to_le_bytes());
    buf.extend_from_slice(vertex_air);
    buf.extend_from_slice(fragment_air);
    buf.extend_from_slice(key.as_bytes());
    fnv1a64(&buf)
}

pub fn seed_slot_name(slot: &[u8]) -> u64 {
    fnv1a64(slot) ^ SLOT_DOMAIN
}

fn program_slot_entry_bytes(slot_len: usize) -> usize {
    (slot_len + PROGRAM_PREFETCH_BYTES).div_ceil(SLOT_PAGE_BYTES) * SLOT_PAGE_BYTES
}

pub(crate) fn parse_stack_hex(text: &str) -> Result<u64> {
    let bytes = hex::decode(text).map_err(|_| PlanError::Invalid(format!("invalid stack hex: {text}")))?;
    if bytes.len() != 8 {
        return Err(PlanError::Invalid(format!("stack hex must be 16 chars: {text}")));
    }
    Ok(u64::from_be_bytes(bytes.try_into().expect("8 bytes")))
}

fn parse_air_hex(text: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(text).map_err(|_| PlanError::Invalid(format!("invalid AIR hex: {text}")))?;
    bytes
        .try_into()
        .map_err(|_| PlanError::Invalid("AIR hash must be 64 hex chars".into()))
}

struct BinReader<'a> {
    data: &'a [u8],
    at: usize,
}

impl<'a> BinReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, at: 0 }
    }

    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let slice = self.data.get(self.at..self.at.checked_add(n)?)?;
        self.at += n;
        Some(slice)
    }

    fn u8(&mut self) -> Option<u8> {
        Some(self.bytes(1)?[0])
    }

    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.bytes(4)?.try_into().ok()?))
    }

    fn u64(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.bytes(8)?.try_into().ok()?))
    }

    fn text(&mut self) -> Option<&'a str> {
        let len = self.u32()? as usize;
        std::str::from_utf8(self.bytes(len)?).ok()
    }

    fn skip_list(&mut self, elem_size: usize) -> Option<usize> {
        let count = self.u32()? as usize;
        let bytes = count.checked_mul(elem_size)?;
        self.bytes(bytes)?;
        Some(count)
    }
}

struct ParsedProgram<'a> {
    image: &'a [u8],
    data: &'a [u8],
}

fn read_captured_program<'a>(r: &mut BinReader<'a>) -> Option<ParsedProgram<'a>> {
    let _id = r.text()?;
    let img_len = r.u32()? as usize;
    let image = r.bytes(img_len)?;
    let data_len = r.u32()? as usize;
    let data = r.bytes(data_len)?;
    r.skip_list(1)?; // cbufTypes
    r.skip_list(2)?; // farUbos
    let _gprs = r.u8()?;
    let _bound = r.u8()?;
    Some(ParsedProgram { image, data })
}

fn read_render_slot_programs<'a>(r: &mut BinReader<'a>, twinned: bool) -> Option<Vec<ParsedProgram<'a>>> {
    let pipe = r.bytes(PACK_PIPELINE_BYTES)?;
    let _key_string = r.text()?;
    let mut slots = Vec::new();
    slots.push(read_captured_program(r)?);
    slots.push(read_captured_program(r)?);
    if pipe[CONTROL_WORD_OFFSET] != 0 {
        let _ = read_captured_program(r)?;
        let _ = read_captured_program(r)?;
    }
    r.skip_list(4)?; // blob
    r.skip_list(8)?; // extras
    r.skip_list(8)?; // restore
    r.skip_list(32)?; // samplers
    let split_bits = r.u8()?;
    let mut split_fs = None;
    if (split_bits & 2) != 0 {
        r.bytes(PACK_SPLIT_BYTES)?;
        split_fs = Some(read_captured_program(r)?);
        let _prologue = read_captured_program(r)?;
    }
    let variants = r.u32()? as usize;
    for _ in 0..variants {
        let _assume = r.u32()?;
        slots.push(read_captured_program(r)?);
    }
    if let Some(fs) = split_fs {
        slots.push(fs);
    }
    r.skip_list(4)?; // spill
    r.skip_list(4)?; // far
    let has_twin = r.u8()?;
    if has_twin != 0 {
        if !twinned {
            return None;
        }
        let _ = read_render_slot_programs(r, false)?;
    }
    Some(slots)
}

fn build_program_slot(image: &[u8], data: &[u8]) -> Vec<u8> {
    let mut slot = vec![0u8; 0x30];
    slot.extend_from_slice(image);
    let aligned = (slot.len() + 0xFF) & !0xFF;
    slot.resize(aligned, 0);
    slot.extend_from_slice(data);
    slot
}

fn parse_and_extract_slots(
    bytes: &[u8],
    want_stack: u64,
    want_vertex_air: &[u8; 32],
    want_fragment_air: &[u8; 32],
    want_key: &str,
) -> Option<(bool, Vec<Vec<u8>>)> {
    let mut r = BinReader::new(bytes);
    if r.bytes(8)? != CACHE_ENTRY_MAGIC {
        return None;
    }
    if r.u32()? != CACHE_ENTRY_FORMAT {
        return None;
    }
    let kind = r.u32()?;
    let stack = r.u64()?;
    let v_air = r.bytes(32)?;
    let f_air = r.bytes(32)?;
    let key = r.text()?;
    if stack != want_stack || v_air != want_vertex_air || f_air != want_fragment_air || key != want_key {
        return None;
    }
    if kind == KIND_KERNEL {
        return Some((true, Vec::new()));
    }
    if kind != KIND_RECORD {
        return None;
    }
    let progs = read_render_slot_programs(&mut r, true)?;
    if r.at != bytes.len() {
        return None;
    }
    let mut slots = Vec::new();
    for p in progs {
        if p.image.is_empty() {
            continue;
        }
        let slot = build_program_slot(p.image, p.data);
        if slot.len() >= SLOT_PAGE_BYTES {
            slots.push(slot);
        }
    }
    Some((false, slots))
}

pub fn restamp_buckets(new_stack_hex: &str, output: &Path, bucket_dirs: &[PathBuf]) -> Result<Value> {
    let new_stack = parse_stack_hex(new_stack_hex)?;
    let stack_str = format!("{new_stack:016x}");
    let rec_dir = output.join(format!("rec-{stack_str}"));
    fs::create_dir_all(&rec_dir)?;

    let mut out_rows: Vec<Value> = Vec::new();
    let mut written: BTreeMap<u64, Vec<u8>> = BTreeMap::new();
    let mut pack_version: Option<u64> = None;

    for bucket_dir in bucket_dirs {
        let manifest_val: Value = serde_json::from_str(&fs::read_to_string(bucket_dir.join("manifest.json"))?)?;
        let obj = manifest_val
            .as_object()
            .ok_or_else(|| PlanError::Invalid("manifest is not an object".into()))?;
        if obj.get("schema").and_then(Value::as_u64) != Some(1)
            || obj.get("cacheVersion").and_then(Value::as_u64) != Some(u64::from(CACHE_ENTRY_FORMAT))
        {
            return Err(PlanError::Invalid(format!(
                "unsupported manifest version in {}",
                bucket_dir.display()
            )));
        }
        let pv = obj
            .get("packVersion")
            .and_then(Value::as_u64)
            .ok_or_else(|| PlanError::Invalid("missing packVersion".into()))?;
        if let Some(prev) = pack_version
            && prev != pv
        {
            return Err(PlanError::Invalid("mismatched packVersion across buckets".into()));
        }
        pack_version = Some(pv);

        let old_stack_str = obj
            .get("stack")
            .and_then(Value::as_str)
            .ok_or_else(|| PlanError::Invalid("missing stack".into()))?;
        let old_stack = parse_stack_hex(old_stack_str)?;
        let records = obj
            .get("records")
            .and_then(Value::as_array)
            .ok_or_else(|| PlanError::Invalid("missing records array".into()))?;

        for row_val in records {
            let mut row = row_val
                .as_object()
                .ok_or_else(|| PlanError::Invalid("record row is not an object".into()))?
                .clone();
            restamp_row(&mut row, bucket_dir, old_stack, new_stack, output, &mut written)?;
            out_rows.push(Value::Object(row));
        }
    }

    let manifest = json!({
        "cacheVersion": CACHE_ENTRY_FORMAT,
        "packVersion": pack_version.unwrap_or(37),
        "records": out_rows,
        "schema": 1,
        "stack": stack_str,
    });
    fs::write(output.join("manifest.json"), serde_json::to_string(&manifest)? + "\n")?;
    Ok(manifest)
}

fn restamp_row(
    row: &mut Map<String, Value>,
    input_dir: &Path,
    old_stack: u64,
    new_stack: u64,
    output_dir: &Path,
    written: &mut BTreeMap<u64, Vec<u8>>,
) -> Result<()> {
    let status = row
        .get("status")
        .and_then(Value::as_str)
        .ok_or_else(|| PlanError::Invalid("row missing status".into()))?;
    let made = status == "made";
    let Some(key_str) = row.get("key").and_then(Value::as_str).map(str::to_owned) else {
        if made || row.contains_key("name") || row.contains_key("artifact") {
            return Err(PlanError::Invalid("invalid keyless row".into()));
        }
        return Ok(());
    };
    let v_air = parse_air_hex(
        row.get("vertexAir")
            .and_then(Value::as_str)
            .ok_or_else(|| PlanError::Invalid("row missing vertexAir".into()))?,
    )?;
    let f_air = parse_air_hex(
        row.get("fragmentAir")
            .and_then(Value::as_str)
            .ok_or_else(|| PlanError::Invalid("row missing fragmentAir".into()))?,
    )?;
    let old_name = cache_key_name(old_stack, &v_air, &f_air, &key_str);
    let old_name_hex = format!("{old_name:016x}");
    if row.get("name").and_then(Value::as_str) != Some(old_name_hex.as_str()) {
        return Err(PlanError::Invalid(format!("row name does not match {old_name_hex}")));
    }
    let new_name = cache_key_name(new_stack, &v_air, &f_air, &key_str);
    let new_name_hex = format!("{new_name:016x}");

    if !made {
        if row.contains_key("artifact") {
            return Err(PlanError::Invalid("unmade row has artifact".into()));
        }
        row.insert("name".to_owned(), Value::String(new_name_hex));
        return Ok(());
    }

    let original_rel = format!("rec-{old_stack:016x}/{old_name_hex}.rec");
    if row.get("artifact").and_then(Value::as_str) != Some(original_rel.as_str()) {
        return Err(PlanError::Invalid("artifact path mismatch".into()));
    }
    let mut bytes = fs::read(input_dir.join(&original_rel))?;
    if bytes.len() > MAX_ENTRY_BYTES {
        return Err(PlanError::Invalid("native record exceeds size limit".into()));
    }
    let (is_kernel, raw_slots) = parse_and_extract_slots(&bytes, old_stack, &v_air, &f_air, &key_str)
        .ok_or_else(|| PlanError::Invalid(format!("invalid SWRAWREC entry {original_rel}")))?;
    if is_kernel && !key_str.starts_with("kernel | ") {
        return Err(PlanError::Invalid("kernel entry missing 'kernel | ' prefix".into()));
    }

    // Only the 8-byte stack at offset 16 changes in the binary SWRAWREC entry.
    bytes[16..24].copy_from_slice(&new_stack.to_le_bytes());
    let new_rel = format!("rec-{new_stack:016x}/{new_name_hex}.rec");
    if let Some(prev) = written.get(&new_name) {
        if prev != &bytes {
            return Err(PlanError::Invalid(format!(
                "conflicting record bytes for {new_name_hex}"
            )));
        }
    } else {
        fs::write(output_dir.join(&new_rel), &bytes)?;
        written.insert(new_name, bytes);
    }

    let mut slot_rows = Vec::new();
    if !is_kernel {
        for mut slot in raw_slots {
            let slot_name = format!("{:016x}", seed_slot_name(&slot));
            slot.resize(program_slot_entry_bytes(slot.len()), 0);
            let slot_rel = format!("rec-{new_stack:016x}/{slot_name}.slot");
            fs::write(output_dir.join(&slot_rel), &slot)?;
            slot_rows.push(json!({"name": slot_name, "path": slot_rel}));
        }
    }
    row.insert("name".to_owned(), Value::String(new_name_hex));
    row.insert("artifact".to_owned(), Value::String(new_rel));
    if slot_rows.is_empty() {
        row.remove("slots");
    } else {
        row.insert("slots".to_owned(), Value::Array(slot_rows));
    }
    Ok(())
}
