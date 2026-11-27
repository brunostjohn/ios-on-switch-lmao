// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet, HashMap};

use crate::iterated_long::iterated_programs;
use crate::iterated_ucode::{IteratedProgram, OGL_ONE, OGL_ZERO};
use crate::packfile::{PipelineEntry, PipelineRecord};
use crate::{LinkError, Result};

const MATRIX: &str = "vcm";
const ROW_WORDS: usize = 10;
const CLAMP_WORDS: usize = 1;
const ROWS_ONLY: &str = "vcm.rows";
const HEADER_BYTES: usize = 0x50;
const LDC_OPCODE: u64 = 0x1DF2;

pub fn ldc_loads(image: &[u8], bank: u8) -> BTreeSet<usize> {
    let mut out = BTreeSet::new();
    if image.len() < HEADER_BYTES + 8 {
        return out;
    }
    for (i, chunk) in image[HEADER_BYTES..].as_chunks::<8>().0.iter().enumerate() {
        if i % 4 == 0 {
            continue;
        }
        let w = u64::from_le_bytes(*chunk);
        if (w >> 51) != LDC_OPCODE || ((w >> 36) & 0x1F) != u64::from(bank) || ((w >> 44) & 3) != 0 {
            continue;
        }
        let count = match (w >> 48) & 7 {
            4 => 1,
            5 => 2,
            6 => 4,
            _ => 0,
        };
        let base = ((w >> 20) & 0xFFFF) as usize;
        for k in 0..count {
            out.insert(base + 4 * k);
        }
    }
    out
}

pub fn fragment_banks(pipe: &PipelineRecord) -> Vec<(u8, u8)> {
    // Preserves Python dict insertion order while updating duplicate slots in place.
    let mut out: Vec<(u8, u8)> = Vec::new();
    for chunk in pipe.uniform.as_chunks::<4>().0 {
        let entry_idx = usize::from(chunk[0]);
        let bank = chunk[1];
        if let Some(PipelineEntry::Ubo { stage, slot, ssbo, .. }) = pipe.entries.get(entry_idx)
            && *stage == 2
            && !*ssbo
        {
            if let Some(existing) = out.iter_mut().find(|(s, _)| *s == *slot) {
                existing.1 = bank;
            } else {
                out.push((*slot, bank));
            }
        }
    }
    out
}

pub fn matrix_uniform(storage: &PipelineRecord, free: &PipelineRecord) -> Option<(u8, u16, u8)> {
    let free_banks: HashMap<u8, u8> = fragment_banks(free).into_iter().collect();
    let mut found = Vec::new();
    let mut shared = Vec::new();
    for (slot, bank) in fragment_banks(storage) {
        let read = ldc_loads(&storage.fs_image, bank);
        let theirs = free_banks
            .get(&slot)
            .map(|&b| ldc_loads(&free.fs_image, b))
            .unwrap_or_default();
        for &offset in &read {
            if offset % 16 != 0 {
                continue;
            }
            let rows: BTreeSet<usize> = (0..ROW_WORDS).map(|k| offset + 4 * k).collect();
            if !rows.is_subset(&read) {
                continue;
            }
            let clamp = offset + 4 * ROW_WORDS;
            let words = ROW_WORDS + usize::from(read.contains(&clamp) && !theirs.contains(&clamp));
            if rows.is_disjoint(&theirs) {
                return Some((slot, offset as u16, words as u8));
            }
            let diff: BTreeSet<usize> = read.difference(&theirs).copied().collect();
            if !diff.is_empty() && diff.iter().all(|o| rows.contains(o) || *o == clamp) {
                found.push((slot, offset as u16, words as u8));
            } else if read == theirs {
                shared.push((slot, offset as u16, words as u8));
            }
        }
    }
    if let Some(&first) = found.first() {
        return Some(first);
    }
    if shared.len() == 1 { Some(shared[0]) } else { None }
}

pub fn attach(
    pipes: Vec<PipelineRecord>,
    report: &mut Vec<String>,
) -> Result<(Vec<PipelineRecord>, Vec<IteratedProgram>)> {
    let mut storage_by_key: HashMap<String, usize> = HashMap::new();
    for (idx, p) in pipes.iter().enumerate() {
        if p.form.is_empty() {
            storage_by_key.insert(p.key.clone(), idx);
        }
    }

    type ProgramKey = (String, Option<u8>, Option<u16>, Option<u8>);
    let table = iterated_programs();
    let mut program_indices: HashMap<ProgramKey, usize> = HashMap::new();
    let mut records: Vec<IteratedProgram> = Vec::new();
    let mut kept_indices: Vec<(usize, u16)> = Vec::with_capacity(pipes.len());
    let mut unplaced = 0usize;

    for (idx, p) in pipes.iter().enumerate() {
        if p.iterated_name.is_empty() {
            kept_indices.push((idx, p.iterated));
            continue;
        }
        let name = p.iterated_name.as_str();
        let (record, identity) = if name == MATRIX {
            let found = storage_by_key
                .get(&p.key)
                .and_then(|&s_idx| matrix_uniform(&pipes[s_idx], p));
            let Some((slot, offset, words)) = found else {
                unplaced += 1;
                continue;
            };
            let rec = IteratedProgram {
                name: if usize::from(words) == ROW_WORDS + CLAMP_WORDS {
                    MATRIX.to_owned()
                } else {
                    ROWS_ONLY.to_owned()
                },
                words: Vec::new(),
                enable: 3,
                constants: BTreeMap::new(),
                alpha_source: OGL_ONE,
                alpha_dest: OGL_ZERO,
                uniform_slot: slot,
                uniform_offset: offset,
                uniform_words: words,
            };
            (rec, (name.to_owned(), Some(slot), Some(offset), Some(words)))
        } else if let Some(prog) = table.get(name) {
            (prog.clone(), (name.to_owned(), None, None, None))
        } else {
            return Err(LinkError::Invalid(format!(
                "a fetch-free form names an iterated blend program iterated/table.py lacks: {name}"
            )));
        };
        let prog_idx = match program_indices.get(&identity) {
            Some(&existing) => existing,
            None => {
                let next = records.len();
                program_indices.insert(identity, next);
                records.push(record);
                next
            }
        };
        kept_indices.push((idx, (prog_idx + 1) as u16));
    }

    let mut pipes_opt: Vec<Option<PipelineRecord>> = pipes.into_iter().map(Some).collect();
    let mut kept = Vec::with_capacity(kept_indices.len());
    for (idx, iterated) in kept_indices {
        let mut p = pipes_opt[idx].take().expect("unique index");
        p.iterated_name.clear();
        p.iterated = iterated;
        p.fs_image.clear();
        kept.push(p);
    }

    let free_count = kept.iter().filter(|p| p.form == "free").count();
    let rung3 = kept.iter().filter(|p| p.form == "free" && p.iterated != 0).count();
    report.push(format!(
        "{free_count} fetch-free forms: {} on rung 2, {rung3} on rung 3 ({} iterated blend programs); {unplaced} colour matrix forms left out: no uniform buffer shows the matrix past its reads",
        free_count - rung3,
        records.len()
    ));
    Ok((kept, records))
}
