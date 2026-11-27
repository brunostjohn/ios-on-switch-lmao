// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;

use crate::kernels::place_compute_program;
use crate::packfile::{PROGRAM_CBUFS, PlacedProgram, place_graphics};
use crate::records::{PACK_SPLIT_BYTES, RawProgram};
use crate::{LinkError, Result};

const QMD_CBUFS: u8 = 8;
const FRONTIER_CBUF_TYPE: u8 = 6;
const MAX_FRONTIER_BYTES: u16 = 0xFFF0;

#[derive(Clone, Debug)]
pub struct SplitRecord {
    pub threshold: u32,
    pub whole: u32,
    pub fs: u32,
    pub prologue: u32,
    pub fs_id: String,
    pub prologue_id: String,
    pub frontier_bytes: u16,
    pub frontier_cbuf: u8,
    pub ubo_cbufs: Vec<u8>,
    pub shader_data_cbuf: u8,
    pub prologue_cbufs: u8,
    pub batch_version: u16,
    pub batch_threads: u16,
    pub qmd: [u32; 64],
    pub cost_version: u16,
    pub cost_reserved: u16,
    pub cost_fields: [u32; 7],
}

pub fn of_record(record: &[u8], fs_id: String, prologue_id: String) -> Result<SplitRecord> {
    if record.len() < PACK_SPLIT_BYTES {
        return Err(LinkError::Invalid(format!(
            "split record truncated: {} bytes (need {PACK_SPLIT_BYTES})",
            record.len()
        )));
    }
    let u32_at = |o: usize| u32::from_le_bytes(record[o..o + 4].try_into().expect("4 bytes"));
    let u16_at = |o: usize| u16::from_le_bytes([record[o], record[o + 1]]);

    let threshold = u32_at(0);
    let frontier_bytes = u16_at(16);
    let frontier_cbuf = record[18];
    let ubos = usize::from(record[19]);
    let shader_data_cbuf = record[20];
    let prologue_cbufs = record[21];
    let ubo_cbufs = record[22..22 + ubos.min(8)].to_vec();
    let batch_version = u16_at(30);
    let batch_threads = u16_at(32);
    let mut qmd = [0u32; 64];
    for (i, slot) in qmd.iter_mut().enumerate() {
        *slot = u32_at(34 + 4 * i);
    }
    let cost_version = u16_at(290);
    let cost_reserved = u16_at(292);
    let mut cost_fields = [0u32; 7];
    for (i, slot) in cost_fields.iter_mut().enumerate() {
        *slot = u32_at(294 + 4 * i);
    }
    Ok(SplitRecord {
        threshold,
        whole: 0,
        fs: 0,
        prologue: 0,
        fs_id,
        prologue_id,
        frontier_bytes,
        frontier_cbuf,
        ubo_cbufs,
        shader_data_cbuf,
        prologue_cbufs,
        batch_version,
        batch_threads,
        qmd,
        cost_version,
        cost_reserved,
        cost_fields,
    })
}

fn usable_frontier(s: &SplitRecord) -> bool {
    s.frontier_bytes > 0 && s.frontier_bytes <= MAX_FRONTIER_BYTES && s.frontier_bytes.is_multiple_of(16)
}

fn instruction_slots(program: &RawProgram) -> usize {
    program.image.len().saturating_sub(80) / 32 * 3
}

fn far_padded(prog: &RawProgram, len: usize) -> Vec<u16> {
    let mut out = prog.far_ubos.clone();
    out.resize(len, 0);
    out
}

fn binds_as_whole(s: &SplitRecord, whole: &str, programs: &HashMap<String, RawProgram>) -> bool {
    let whole_prog = &programs[whole];
    let fs_prog = &programs[&s.fs_id];
    let types = &whole_prog.cbufs;
    if usize::from(s.frontier_cbuf) != types.len() {
        return false;
    }
    if fs_prog.cbufs.len() != types.len() + 1
        || &fs_prog.cbufs[..types.len()] != types.as_slice()
        || fs_prog.cbufs[types.len()] != FRONTIER_CBUF_TYPE
    {
        return false;
    }
    far_padded(fs_prog, types.len()) == far_padded(whole_prog, types.len())
}

#[allow(clippy::too_many_arguments)]
pub fn lay_out(
    used: &[String],
    found: &HashMap<String, SplitRecord>,
    programs: &HashMap<String, RawProgram>,
    heap: &mut Vec<u8>,
    prog_index: &mut HashMap<String, usize>,
    storage_index: &mut HashMap<String, usize>,
    prog_recs: &mut Vec<PlacedProgram>,
    report: &mut Vec<String>,
) -> Vec<SplitRecord> {
    let mut out = Vec::new();
    let mut before = 0usize;
    let mut after = 0usize;
    for whole in used {
        let Some(s) = found.get(whole) else {
            continue;
        };
        if !programs.contains_key(&s.fs_id) || !programs.contains_key(&s.prologue_id) {
            continue;
        }
        if programs[&s.fs_id].cbufs.len() > PROGRAM_CBUFS || s.prologue_cbufs > QMD_CBUFS {
            continue;
        }
        if !binds_as_whole(s, whole, programs) {
            continue;
        }
        if !usable_frontier(s) {
            report.push(format!(
                "{whole}: split left out, a frontier of {} bytes the runtime refuses",
                s.frontier_bytes
            ));
            continue;
        }
        place_graphics(&s.fs_id, programs, heap, prog_index, storage_index, prog_recs);
        let prologue_idx = place_compute_program(&s.prologue_id, programs, heap, prog_index, prog_recs);
        let mut placed = s.clone();
        placed.whole = prog_index[whole] as u32;
        placed.fs = prog_index[&s.fs_id] as u32;
        placed.prologue = prologue_idx as u32;
        out.push(placed);
        before += instruction_slots(&programs[whole]);
        after += instruction_slots(&programs[&s.fs_id]);
    }
    out.sort_by_key(|s| s.whole);
    let pct = if before > 0 {
        format!(
            " ({:+.1}%)",
            100.0 * ((after as f64) - (before as f64)) / (before as f64)
        )
    } else {
        String::new()
    };
    report.push(format!(
        "{} fragment programs split: {before} -> {after} instruction slots{pct}",
        out.len()
    ));
    out
}
