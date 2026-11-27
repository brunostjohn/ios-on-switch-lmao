// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::Path;

use crate::{LinkError, Result};

pub const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
pub const FNV_PRIME: u64 = 0x0100_0000_01b3;
pub const PACK_PIPELINE_BYTES: usize = 736;
pub const PACK_SPLIT_BYTES: usize = 322;
const MAX_VARIANTS_PER_STATE: usize = 4096;
const MAX_TESS_PROGRAMS: usize = 4;

pub fn fnv1a64_seeded(bytes: &[u8], mut h: u64) -> u64 {
    for &b in bytes {
        h = (h ^ u64::from(b)).wrapping_mul(FNV_PRIME);
    }
    h
}

pub fn fnv1a64(bytes: &[u8]) -> u64 {
    fnv1a64_seeded(bytes, FNV_OFFSET_BASIS)
}

#[derive(Clone, Debug)]
pub struct RawProgram {
    pub image: Vec<u8>,
    pub data: Vec<u8>,
    pub cbufs: Vec<u8>,
    pub far_ubos: Vec<u16>,
    pub gprs: u8,
    pub bound_textures: u8,
    pub facts: u32,
    pub identity: Vec<u8>,
    pub storage_id: Option<String>,
}

impl RawProgram {
    pub fn program_hash(&self) -> u64 {
        let h = fnv1a64(&self.image);
        if self.data.is_empty() {
            h
        } else {
            fnv1a64_seeded(&self.data, h)
        }
    }

    pub fn program_name(&self) -> String {
        format!("{:x}", self.program_hash())
    }

    pub fn program_id_u64(&self) -> u64 {
        let mut h = self.program_hash();
        if !self.cbufs.is_empty() {
            h = fnv1a64_seeded(&self.cbufs, h);
        }
        if !self.identity.is_empty() {
            h = fnv1a64_seeded(&self.identity, h);
        }
        if !self.far_ubos.is_empty() {
            let mut far = Vec::with_capacity(self.far_ubos.len() * 2);
            for &u in &self.far_ubos {
                far.extend_from_slice(&u.to_le_bytes());
            }
            h = fnv1a64_seeded(&far, h);
        }
        h
    }

    pub fn program_id(&self) -> String {
        format!("{:x}", self.program_id_u64())
    }
}

#[derive(Clone, Debug)]
pub struct RawSplit {
    pub record: Vec<u8>,
    pub programs: [RawProgram; 2],
}

#[derive(Clone, Debug)]
pub struct RawVariant {
    pub assume: u32,
    pub program: RawProgram,
    pub memory: u32,
}

#[derive(Clone, Debug)]
pub struct RawBound {
    pub mask: u32,
    pub push: Vec<u32>,
    pub spill: [Vec<u32>; 2],
}

#[derive(Clone, Debug)]
pub struct RawStateRecord {
    pub outcome: u32,
    pub key: String,
    pub why: String,
    pub form: String,
    pub tier: u32,
    pub pipeline: Vec<u8>,
    pub key_string: String,
    pub regs: HashMap<u32, u32>,
    pub programs: [RawProgram; 2],
    pub samplers: Vec<[u32; 8]>,
    pub split: Option<RawSplit>,
    pub variants: Vec<RawVariant>,
    pub iterated: String,
    pub spill: Vec<u32>,
    pub bound: Option<RawBound>,
    pub tess_programs: Vec<RawProgram>,
    pub extras: Vec<(u32, u32)>,
    pub restore: Vec<(u32, u32)>,
    pub far: Vec<u32>,
}

#[derive(Clone, Debug)]
pub struct RawKernelRecord {
    pub outcome: u32,
    pub key: String,
    pub why: String,
    pub kernel: Vec<u8>,
    pub program: Option<RawProgram>,
    pub program_id: String,
    pub samplers: Vec<[u32; 8]>,
    pub spill: Vec<u32>,
    pub facts: Option<(u32, u32, u32)>,
}

#[derive(Clone, Debug, Default)]
pub struct BucketRecords {
    pub states: Vec<RawStateRecord>,
    pub kernels: Vec<RawKernelRecord>,
    pub reflections: BTreeMap<(bool, u64), Vec<u8>>,
    pub fast: Vec<(u64, u64, u64)>,
}

struct Reader<'a> {
    data: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, at: 0 }
    }

    fn done(&self) -> bool {
        self.at >= self.data.len()
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        let end = self
            .at
            .checked_add(n)
            .ok_or_else(|| LinkError::Invalid("records.bin offset overflow".into()))?;
        let slice = self
            .data
            .get(self.at..end)
            .ok_or_else(|| LinkError::Invalid(format!("records.bin truncated at byte {}", self.at)))?;
        self.at = end;
        Ok(slice)
    }

    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().expect("4 bytes")))
    }

    fn blob(&mut self) -> Result<&'a [u8]> {
        let n = self.u32()? as usize;
        self.take(n)
    }

    fn utf8_blob(&mut self) -> Result<String> {
        let bytes = self.blob()?;
        std::str::from_utf8(bytes)
            .map(str::to_owned)
            .map_err(|e| LinkError::Invalid(format!("invalid UTF-8 in records.bin: {e}")))
    }

    fn words(&mut self) -> Result<Vec<u32>> {
        let raw = self.blob()?;
        Ok(raw.as_chunks::<4>().0.iter().map(|c| u32::from_le_bytes(*c)).collect())
    }

    fn headers(&mut self) -> Result<Vec<[u32; 8]>> {
        let raw = self.blob()?;
        let mut out = Vec::with_capacity(raw.len() / 32);
        for chunk in raw.as_chunks::<32>().0 {
            let mut h = [0u32; 8];
            for (i, slot) in h.iter_mut().enumerate() {
                *slot = u32::from_le_bytes(chunk[4 * i..4 * i + 4].try_into().expect("4 bytes"));
            }
            out.push(h);
        }
        Ok(out)
    }

    fn program(&mut self) -> Result<RawProgram> {
        let image = self.blob()?.to_vec();
        let data = self.blob()?.to_vec();
        let cbufs = self.blob()?.to_vec();
        let far_raw = self.blob()?;
        let far_ubos: Vec<u16> = far_raw
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u16::from_le_bytes(*c))
            .collect();
        let words = self.u32()?;
        Ok(RawProgram {
            image,
            data,
            cbufs,
            far_ubos,
            gprs: (words & 0xFF) as u8,
            bound_textures: ((words >> 8) & 0xFF) as u8,
            facts: words >> 16,
            identity: Vec::new(),
            storage_id: None,
        })
    }
}

fn empty_program() -> RawProgram {
    RawProgram {
        image: Vec::new(),
        data: Vec::new(),
        cbufs: Vec::new(),
        far_ubos: Vec::new(),
        gprs: 0,
        bound_textures: 0,
        facts: 0,
        identity: Vec::new(),
        storage_id: None,
    }
}

pub fn read_records(path: &Path) -> Result<BucketRecords> {
    let bytes = fs::read(path)?;
    read_records_bytes(&bytes, &path.display().to_string())
}

pub fn read_records_bytes(bytes: &[u8], source: &str) -> Result<BucketRecords> {
    let mut r = Reader::new(bytes);
    let mut out = BucketRecords::default();
    while !r.done() {
        let tag = r.take(4)?;
        if tag == b"DFKY" {
            let blob = r.blob()?;
            if blob.len() != 24 {
                return Err(LinkError::Invalid(format!("{source}: bad DFKY size {}", blob.len())));
            }
            let fast = u64::from_le_bytes(blob[0..8].try_into().expect("8 bytes"));
            let state = u64::from_le_bytes(blob[8..16].try_into().expect("8 bytes"));
            let tier = u64::from_le_bytes(blob[16..24].try_into().expect("8 bytes"));
            out.fast.push((fast, state, tier));
            continue;
        }
        if tag == b"DRFL" {
            let compute = r.u32()? != 0;
            let key = r.utf8_blob()?;
            let blob = r.blob()?.to_vec();
            let state = if compute {
                key.split_once(" | tg=").map_or(key.as_str(), |(a, _)| a)
            } else {
                key.as_str()
            };
            out.reflections
                .entry((compute, fnv1a64(state.as_bytes())))
                .or_insert(blob);
            continue;
        }
        if tag != b"DREC" && tag != b"DKRN" {
            return Err(LinkError::Invalid(format!(
                "{source}: no record at byte {}",
                r.at.saturating_sub(4)
            )));
        }
        let outcome = r.u32()?;
        let key = r.utf8_blob()?;
        let why = r.utf8_blob()?;
        if tag == b"DKRN" {
            let mut krec = RawKernelRecord {
                outcome,
                key,
                why,
                kernel: Vec::new(),
                program: None,
                program_id: String::new(),
                samplers: Vec::new(),
                spill: Vec::new(),
                facts: None,
            };
            if outcome == 0 {
                krec.kernel = r.blob()?.to_vec();
                let code = r.blob()?;
                let mut image = vec![0u8; 80];
                image.extend_from_slice(code);
                let data = r.blob()?.to_vec();
                let cbufs = r.blob()?.to_vec();
                krec.program = Some(RawProgram {
                    image,
                    data,
                    cbufs,
                    far_ubos: Vec::new(),
                    gprs: 0,
                    bound_textures: 0,
                    facts: 0,
                    identity: Vec::new(),
                    storage_id: None,
                });
                krec.samplers = r.headers()?;
                krec.spill = r.words()?;
                let facts_raw = r.blob()?;
                if facts_raw.len() != 12 {
                    return Err(LinkError::Invalid(format!("{source}: bad DKRN facts")));
                }
                krec.facts = Some((
                    u32::from_le_bytes(facts_raw[0..4].try_into().expect("4")),
                    u32::from_le_bytes(facts_raw[4..8].try_into().expect("4")),
                    u32::from_le_bytes(facts_raw[8..12].try_into().expect("4")),
                ));
            }
            out.kernels.push(krec);
            continue;
        }

        let mut rec = RawStateRecord {
            outcome,
            key,
            why,
            form: String::new(),
            tier: 0,
            pipeline: Vec::new(),
            key_string: String::new(),
            regs: HashMap::new(),
            programs: [empty_program(), empty_program()],
            samplers: Vec::new(),
            split: None,
            variants: Vec::new(),
            iterated: String::new(),
            spill: Vec::new(),
            bound: None,
            tess_programs: Vec::new(),
            extras: Vec::new(),
            restore: Vec::new(),
            far: Vec::new(),
        };
        if outcome == 0 {
            rec.pipeline = r.blob()?.to_vec();
            if rec.pipeline.len() != PACK_PIPELINE_BYTES {
                return Err(LinkError::Invalid(format!(
                    "{source}: bad DREC pipeline size {} (expected {PACK_PIPELINE_BYTES})",
                    rec.pipeline.len()
                )));
            }
            rec.key_string = r.utf8_blob()?;
            let reg_words = r.words()?;
            rec.regs.reserve(reg_words.len() / 2);
            for pair in reg_words.as_chunks::<2>().0 {
                rec.regs.insert(pair[0], pair[1]);
            }
            rec.programs = [r.program()?, r.program()?];
            rec.samplers = r.headers()?;
            if r.u32()? != 0 {
                let split_rec = r.blob()?.to_vec();
                if split_rec.len() != PACK_SPLIT_BYTES {
                    return Err(LinkError::Invalid(format!(
                        "{source}: bad DREC split size {} (expected {PACK_SPLIT_BYTES})",
                        split_rec.len()
                    )));
                }
                let split_progs = [r.program()?, r.program()?];
                rec.split = Some(RawSplit {
                    record: split_rec,
                    programs: split_progs,
                });
            }
            let var_count = r.u32()? as usize;
            if var_count > MAX_VARIANTS_PER_STATE {
                return Err(LinkError::Invalid(format!(
                    "{source}: variant count {var_count} exceeds limit {MAX_VARIANTS_PER_STATE}"
                )));
            }
            let rem = r.data.len().saturating_sub(r.at);
            rec.variants.reserve(var_count.min(rem / 24));
            for _ in 0..var_count {
                let assume = r.u32()?;
                let prog = r.program()?;
                let memory = r.u32()?;
                rec.variants.push(RawVariant {
                    assume,
                    program: prog,
                    memory,
                });
            }
            rec.iterated = r.utf8_blob()?;
            rec.spill = r.words()?;
            let mask = r.u32()?;
            let push = r.words()?;
            let spill0 = r.words()?;
            let spill1 = r.words()?;
            rec.bound = Some(RawBound {
                mask,
                push,
                spill: [spill0, spill1],
            });
            let tess_count = r.u32()? as usize;
            if tess_count > MAX_TESS_PROGRAMS {
                return Err(LinkError::Invalid(format!(
                    "{source}: tessellation program count {tess_count} exceeds limit {MAX_TESS_PROGRAMS}"
                )));
            }
            rec.tess_programs.reserve(tess_count);
            for _ in 0..tess_count {
                rec.tess_programs.push(r.program()?);
            }
            let extras_w = r.words()?;
            let restore_w = r.words()?;
            rec.extras = extras_w.as_chunks::<2>().0.iter().map(|c| (c[0], c[1])).collect();
            rec.restore = restore_w.as_chunks::<2>().0.iter().map(|c| (c[0], c[1])).collect();
            rec.far = r.words()?;
        } else if outcome > 2 {
            return Err(LinkError::Invalid(format!("{source}: invalid DREC outcome {outcome}")));
        }
        out.states.push(rec);
    }
    Ok(out)
}

pub fn fast_keys(lines: &[(u64, u64, u64)]) -> Result<(HashMap<u64, u64>, HashMap<u64, u32>)> {
    let mut by_fast: HashMap<u64, u64> = HashMap::new();
    let mut by_state: HashMap<u64, u64> = HashMap::new();
    let mut tiers: HashMap<u64, u32> = HashMap::new();
    for &(fast, state, tier) in lines {
        if let Some(&prev) = by_fast.get(&fast) {
            if prev != state {
                return Err(LinkError::Invalid(format!(
                    "fast key {fast:016x} names states {prev:016x} and {state:016x}"
                )));
            }
        } else {
            by_fast.insert(fast, state);
        }
        if let Some(&prev) = by_state.get(&state) {
            if prev != fast {
                return Err(LinkError::Invalid(format!(
                    "state {state:016x} has fast keys {prev:016x} and {fast:016x}"
                )));
            }
        } else {
            by_state.insert(state, fast);
        }
        let t = tier as u32;
        tiers.entry(state).and_modify(|cur| *cur = (*cur).min(t)).or_insert(t);
    }
    Ok((by_fast, tiers))
}
