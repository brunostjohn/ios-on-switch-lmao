// SPDX-License-Identifier: GPL-3.0-or-later
//! The journal: 16 sectors just below the FAT's 16 MiB start, one header sector and a 15-sector body.
//!
//! A step commits as: body (its sequence number, then its records), barrier, header (sequence, phase, the body's
//! length and CRC), barrier. A header whose body matches is a step that may be partly applied; applying it again
//! finishes it. A header whose body doesn't match is a finished step: the next step's body only goes down once a step
//! is finished, and its header only once that body is durable. Before the first step, `begin` writes step 0's header
//! over the body's zeros; `clear` zeroes the body before the header.

mod plan;
pub mod record;

pub use plan::{Fingerprint, FsKind, Plan};
pub use record::{FieldKind, Record, Step};

use switch_card::{Device, SECTOR};

use crate::error::{Error, Result};

pub const FIRST: u64 = 32752;
pub const SECTORS: u64 = 16;
const MAGIC: &[u8; 8] = b"SwPartJ1";
const VERSION: u32 = 1;
pub const BODY_BYTES: usize = (SECTORS as usize - 1) * SECTOR;
const PLAN_AT: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Relocate,
    Shrink,
    Tables,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    pub seq: u64,
    pub phase: Phase,
    pub plan: Plan,
    body_len: u32,
    body_crc: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    Empty,
    /// The step committed, and may be partly applied.
    Pending(Header, Step),
    /// The step finished; the next one never committed.
    Finished(Header),
}

impl Phase {
    fn code(self) -> u32 {
        match self {
            Phase::Relocate => 1,
            Phase::Shrink => 2,
            Phase::Tables => 3,
        }
    }

    fn from_code(code: u32) -> Option<Phase> {
        [Phase::Relocate, Phase::Shrink, Phase::Tables]
            .into_iter()
            .find(|p| p.code() == code)
    }
}

impl Header {
    fn encode(&self) -> [u8; SECTOR] {
        let mut h = [0u8; SECTOR];
        h[..8].copy_from_slice(MAGIC);
        h[8..12].copy_from_slice(&VERSION.to_le_bytes());
        h[16..24].copy_from_slice(&self.seq.to_le_bytes());
        h[24..28].copy_from_slice(&self.phase.code().to_le_bytes());
        h[28..32].copy_from_slice(&self.body_len.to_le_bytes());
        h[32..36].copy_from_slice(&self.body_crc.to_le_bytes());
        self.plan.encode(&mut h[PLAN_AT..]);
        let crc = crc32fast::hash(&h);
        h[12..16].copy_from_slice(&crc.to_le_bytes());
        h
    }

    fn decode(h: &[u8; SECTOR]) -> Option<Header> {
        let u32_at = |at: usize| u32::from_le_bytes(h[at..at + 4].try_into().unwrap());
        let mut zeroed = *h;
        zeroed[12..16].fill(0);
        if &h[..8] != MAGIC || u32_at(8) != VERSION || crc32fast::hash(&zeroed) != u32_at(12) {
            return None;
        }
        Some(Header {
            seq: u64::from_le_bytes(h[16..24].try_into().unwrap()),
            phase: Phase::from_code(u32_at(24))?,
            plan: Plan::decode(&h[PLAN_AT..])?,
            body_len: u32_at(28),
            body_crc: u32_at(32),
        })
    }
}

pub fn read<D: Device>(dev: &mut D) -> Result<State> {
    if dev.sector_size() != SECTOR as u32 {
        return Err(switch_card::Error::SectorSize(dev.sector_size()).into());
    }
    if dev.sector_count() < FIRST + SECTORS {
        return Err(switch_card::Error::TooSmall(dev.sector_count()).into());
    }
    let mut area = vec![0u8; SECTORS as usize * SECTOR];
    dev.read_sectors(FIRST, &mut area)?;
    let (head, body) = area.split_at(SECTOR);
    if area.iter().all(|&b| b == 0) {
        return Ok(State::Empty);
    }
    let header = Header::decode(head.try_into().unwrap())
        .ok_or(Error::Journal("holds data that is not this installer's journal"))?;
    if header.body_len == 0 && header.body_crc == 0 {
        return Ok(State::Finished(header));
    }
    let len = header.body_len as usize;
    let whole = (8..=BODY_BYTES).contains(&len)
        && crc32fast::hash(&body[..len]) == header.body_crc
        && body[..8] == header.seq.to_le_bytes();
    if !whole {
        return Ok(State::Finished(header));
    }
    let step = Step::decode(&body[8..len])?;
    Ok(State::Pending(header, step))
}

/// Whether `step` fits in the body.
pub fn fits(step: &Step) -> bool {
    8 + step.encoded_len() <= BODY_BYTES
}

/// Step 0: nothing to do yet, marked finished. The area must be empty.
pub fn begin<D: Device>(dev: &mut D, plan: &Plan) -> Result<()> {
    finish(dev, 0, Phase::Relocate, plan)
}

/// Marks the step at `phase` finished so a cut before the next commit will not replay it.
pub fn finish<D: Device>(dev: &mut D, seq: u64, phase: Phase, plan: &Plan) -> Result<()> {
    let header = Header {
        seq,
        phase,
        plan: plan.clone(),
        body_len: 0,
        body_crc: 0,
    };
    put(dev, FIRST, &header.encode())
}

pub fn commit<D: Device>(dev: &mut D, seq: u64, phase: Phase, plan: &Plan, step: &Step) -> Result<()> {
    assert!(fits(step), "the planner keeps every step inside the journal body");
    let mut body = seq.to_le_bytes().to_vec();
    step.encode(&mut body);
    let header = Header {
        seq,
        phase,
        plan: plan.clone(),
        body_len: body.len() as u32,
        body_crc: crc32fast::hash(&body),
    };
    body.resize(BODY_BYTES, 0);
    put(dev, FIRST + 1, &body)?;
    put(dev, FIRST, &header.encode())
}

pub fn clear<D: Device>(dev: &mut D) -> Result<()> {
    put(dev, FIRST + 1, &[0; BODY_BYTES])?;
    put(dev, FIRST, &[0; SECTOR])
}

fn put<D: Device>(dev: &mut D, lba: u64, data: &[u8]) -> Result<()> {
    dev.write_sectors(lba, data)?;
    dev.barrier()?;
    Ok(())
}
