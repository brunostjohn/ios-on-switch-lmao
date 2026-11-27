// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::{PlanError, Result};

pub const SEED_PACK_MAGIC: &[u8; 8] = b"SWSEEDPK";
pub const SEED_PACK_VERSION: u32 = 1;
pub const MAX_PREFIX_BITS: u32 = 24;
pub const MIN_NAME_BYTES: u32 = 4;
pub const MAX_NAME_BYTES: u32 = 64;
pub const HEADER_BYTES: usize = 64;
pub const IDENTITY_BYTES: usize = 32;
pub const ENTRY_TAIL_BYTES: usize = 12;
pub const PAGE_BYTES: u64 = 16384;
pub const DEFAULT_BUCKET_BYTES: u64 = 12 << 20;
const TIERS: usize = 2;
const ALIGNMENT: u64 = 8;
const ENTRIES_PER_RUN: usize = 4;

#[derive(Clone, Debug)]
pub enum EntrySource {
    Path(PathBuf),
    Bytes(Vec<u8>),
}

#[derive(Clone, Debug)]
pub struct SeedPackEntry {
    pub name: Vec<u8>,
    pub tier: u32,
    pub source: EntrySource,
}

fn be32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn le32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn le64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes[..8].try_into().expect("8 bytes"))
}

fn alignment_of(length: u64) -> u64 {
    if length != 0 && length.is_multiple_of(PAGE_BYTES) {
        PAGE_BYTES
    } else {
        ALIGNMENT
    }
}

fn prefix_bits(entries: usize) -> u32 {
    let mut bits = 0u32;
    while bits < MAX_PREFIX_BITS && (1usize << bits) <= entries / ENTRIES_PER_RUN {
        bits += 1;
    }
    bits
}

fn pack_path(dir: &Path, number: u32) -> PathBuf {
    dir.join("packs").join(format!("{number:03}"))
}

fn replace_file(path: &Path, bytes: &[u8]) -> Result<()> {
    static SERIAL: AtomicU64 = AtomicU64::new(0);
    let seq = SERIAL.fetch_add(1, Ordering::Relaxed);
    let mut tmp_name = path.as_os_str().to_os_string();
    tmp_name.push(format!(".{}-{seq}.tmp", std::process::id()));
    let tmp = PathBuf::from(tmp_name);
    fs::write(&tmp, bytes)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(&tmp, fs::Permissions::from_mode(0o644));
    }
    #[cfg(windows)]
    {
        if path.exists() {
            let _ = fs::remove_file(path);
        }
    }
    if let Err(err) = fs::rename(&tmp, path) {
        let _ = fs::remove_file(&tmp);
        return Err(err.into());
    }
    Ok(())
}

pub fn write_seed_pack(
    directory: &Path,
    name_bytes: u32,
    identity: &[u8; IDENTITY_BYTES],
    mut entries: Vec<SeedPackEntry>,
    bucket_bytes: u64,
) -> Result<()> {
    if bucket_bytes == 0 {
        return Err(PlanError::Invalid("no bucket size".into()));
    }
    if !(MIN_NAME_BYTES..=MAX_NAME_BYTES).contains(&name_bytes) {
        return Err(PlanError::Invalid("invalid name_bytes".into()));
    }
    entries.sort_by(|a, b| a.name.cmp(&b.name));

    #[derive(Clone, Copy, Default)]
    struct Placed {
        length: u64,
        pack: u32,
        offset: u32,
    }

    let mut placed = vec![Placed::default(); entries.len()];
    let mut tier_bytes = [0u64; TIERS];
    let mut tier_counts = [0usize; TIERS];
    for (i, entry) in entries.iter().enumerate() {
        if entry.name.len() != name_bytes as usize
            || (entry.tier as usize) >= TIERS
            || (i > 0 && entry.name == entries[i - 1].name)
        {
            return Err(PlanError::Invalid("a misnamed, duplicate or untiered entry".into()));
        }
        let len = match &entry.source {
            EntrySource::Path(p) => {
                let meta = fs::metadata(p)?;
                if !meta.is_file() {
                    return Err(PlanError::Invalid(format!("unreadable: {}", p.display())));
                }
                meta.len()
            }
            EntrySource::Bytes(b) => b.len() as u64,
        };
        if len > u64::from(u32::MAX) {
            return Err(PlanError::Invalid("an entry past 4 GiB".into()));
        }
        placed[i].length = len;
        tier_counts[entry.tier as usize] += 1;
        // Mirrors SwitchSeedPackWriter.h:111: rounds by alignmentOf(len) divided by kAlignment.
        tier_bytes[entry.tier as usize] += (len + alignment_of(len) - 1) / ALIGNMENT * ALIGNMENT;
    }

    let mut tier_packs = [0u32; TIERS];
    let mut tier_first = [0u32; TIERS];
    let mut packs = 0u32;
    for tier in 0..TIERS {
        tier_packs[tier] = if tier_counts[tier] == 0 {
            0
        } else {
            tier_bytes[tier].div_ceil(bucket_bytes).max(1) as u32
        };
        tier_first[tier] = packs;
        packs += tier_packs[tier];
    }

    let mut members: Vec<Vec<usize>> = vec![Vec::new(); packs as usize];
    for (i, entry) in entries.iter().enumerate() {
        let tier = entry.tier as usize;
        let pack = tier_first[tier] + ((u64::from(be32(&entry.name)) * u64::from(tier_packs[tier])) >> 32) as u32;
        placed[i].pack = pack;
        members[pack as usize].push(i);
    }

    fs::create_dir_all(directory.join("packs"))?;
    let mut index = Vec::with_capacity(HEADER_BYTES + 8 * packs as usize);
    index.extend_from_slice(SEED_PACK_MAGIC);
    index.extend_from_slice(&SEED_PACK_VERSION.to_le_bytes());
    index.extend_from_slice(&name_bytes.to_le_bytes());
    index.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    index.extend_from_slice(&packs.to_le_bytes());
    index.extend_from_slice(&tier_packs[0].to_le_bytes());
    let bits = prefix_bits(entries.len());
    index.extend_from_slice(&bits.to_le_bytes());
    index.extend_from_slice(identity);

    let mut pack_buf = Vec::new();
    for number in 0..packs {
        pack_buf.clear();
        for &i in &members[number as usize] {
            let bytes = match &entries[i].source {
                EntrySource::Path(p) => fs::read(p)?,
                EntrySource::Bytes(b) => b.clone(),
            };
            if bytes.len() as u64 != placed[i].length {
                return Err(PlanError::Invalid("entry length changed while packing".into()));
            }
            let align = alignment_of(bytes.len() as u64) as usize;
            let padded_start = pack_buf.len().div_ceil(align) * align;
            pack_buf.resize(padded_start, 0);
            if (pack_buf.len() + bytes.len()) as u64 > u64::from(u32::MAX) {
                return Err(PlanError::Invalid("a pack past 4 GiB".into()));
            }
            placed[i].offset = pack_buf.len() as u32;
            pack_buf.extend_from_slice(&bytes);
            let aligned_end = pack_buf.len().div_ceil(ALIGNMENT as usize) * ALIGNMENT as usize;
            pack_buf.resize(aligned_end, 0);
        }
        replace_file(&pack_path(directory, number), &pack_buf)?;
        index.extend_from_slice(&(pack_buf.len() as u64).to_le_bytes());
    }

    let mut stale = packs;
    while fs::remove_file(pack_path(directory, stale)).is_ok() {
        stale += 1;
    }

    let run_count = 1u64 << bits;
    let mut at = 0usize;
    for run in 0..=run_count {
        while bits != 0 && at < entries.len() && u64::from(be32(&entries[at].name) >> (32 - bits)) < run {
            at += 1;
        }
        let val = if run == run_count {
            entries.len() as u32
        } else if bits != 0 {
            at as u32
        } else {
            0
        };
        index.extend_from_slice(&val.to_le_bytes());
    }
    for (i, entry) in entries.iter().enumerate() {
        index.extend_from_slice(&entry.name);
        index.extend_from_slice(&placed[i].pack.to_le_bytes());
        index.extend_from_slice(&placed[i].offset.to_le_bytes());
        index.extend_from_slice(&(placed[i].length as u32).to_le_bytes());
    }
    replace_file(&directory.join("index"), &index)
}

pub fn parse_seedpack_list(text: &str) -> Result<Vec<SeedPackEntry>> {
    let mut entries = Vec::new();
    for line in text.lines() {
        let mut parts = line.splitn(3, '\t');
        let (Some(tier_s), Some(name_hex), Some(path_s)) = (parts.next(), parts.next(), parts.next()) else {
            return Err(PlanError::Invalid(format!("bad seedpack list line: {line}")));
        };
        let tier = match tier_s {
            "0" => 0,
            "1" => 1,
            _ => return Err(PlanError::Invalid(format!("bad tier in line: {line}"))),
        };
        let name = hex::decode(name_hex).map_err(|e| PlanError::Invalid(format!("bad hex name {name_hex}: {e}")))?;
        entries.push(SeedPackEntry {
            name,
            tier,
            source: EntrySource::Path(PathBuf::from(path_s)),
        });
    }
    Ok(entries)
}

pub fn observed_packs(index_path: &Path) -> Result<u32> {
    let data = fs::read(index_path)?;
    if data.len() < 32 {
        return Err(PlanError::Invalid(format!(
            "{}: shorter than a seed pack header",
            index_path.display()
        )));
    }
    if &data[..8] != SEED_PACK_MAGIC || le32(&data[8..12]) != SEED_PACK_VERSION {
        return Err(PlanError::Invalid(format!(
            "{}: not a version {SEED_PACK_VERSION} seed pack index",
            index_path.display()
        )));
    }
    let packs = le32(&data[20..24]);
    let observed = le32(&data[24..28]);
    if observed > packs {
        return Err(PlanError::Invalid(format!(
            "{}: observed packs {observed} > total packs {packs}",
            index_path.display()
        )));
    }
    Ok(observed)
}

pub fn staged_in_pack(directory: &Path, relative: &str) -> Result<bool> {
    if relative == "index" {
        return Ok(true);
    }
    let Some(rest) = relative.strip_prefix("packs/") else {
        return Ok(false);
    };
    if rest.contains('/') || rest.is_empty() || !rest.bytes().all(|b| b.is_ascii_digit()) {
        return Ok(false);
    }
    let idx: u32 = rest
        .parse()
        .map_err(|_| PlanError::Invalid(format!("invalid pack number: {relative}")))?;
    Ok(idx < observed_packs(&directory.join("index"))?)
}

pub struct SeedPackReader {
    index: Vec<u8>,
    packs: Vec<Option<Vec<u8>>>,
    runs_offset: usize,
    table_offset: usize,
    name_bytes: usize,
    entries: u32,
    observed: u32,
    bits: u32,
}

impl SeedPackReader {
    pub fn open(directory: &Path, name_bytes: u32, identity: Option<&[u8; IDENTITY_BYTES]>) -> Result<Self> {
        let index = fs::read(directory.join("index"))?;
        if index.len() < HEADER_BYTES
            || &index[..8] != SEED_PACK_MAGIC
            || le32(&index[8..12]) != SEED_PACK_VERSION
            || le32(&index[12..16]) != name_bytes
            || !(MIN_NAME_BYTES..=MAX_NAME_BYTES).contains(&name_bytes)
        {
            return Err(PlanError::Invalid("invalid seed pack header".into()));
        }
        let entries = le32(&index[16..20]);
        let pack_count = le32(&index[20..24]);
        let observed = le32(&index[24..28]);
        let bits = le32(&index[28..32]);
        if bits > MAX_PREFIX_BITS || observed > pack_count {
            return Err(PlanError::Invalid("invalid seed pack counts".into()));
        }
        if let Some(want) = identity
            && &index[32..64] != want
        {
            return Err(PlanError::Invalid("seed pack identity mismatch".into()));
        }
        let runs = (1usize << bits) + 1;
        let runs_offset = HEADER_BYTES + 8 * pack_count as usize;
        let table_offset = runs_offset + 4 * runs;
        let expected_size = table_offset + (entries as usize) * (name_bytes as usize + ENTRY_TAIL_BYTES);
        if expected_size != index.len() || le32(&index[runs_offset + 4 * (runs - 1)..]) != entries {
            return Err(PlanError::Invalid("truncated or inconsistent seed pack index".into()));
        }
        let mut packs = vec![None; pack_count as usize];
        for (i, slot) in packs.iter_mut().enumerate() {
            let p = pack_path(directory, i as u32);
            match fs::read(&p) {
                Ok(bytes) => {
                    let expected = le64(&index[HEADER_BYTES + 8 * i..HEADER_BYTES + 8 * (i + 1)]);
                    if bytes.len() as u64 != expected {
                        return Err(PlanError::Invalid(format!("{}: size mismatch", p.display())));
                    }
                    *slot = Some(bytes);
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                    if (i as u32) >= observed {
                        break;
                    }
                }
                Err(e) => return Err(e.into()),
            }
        }
        Ok(Self {
            index,
            packs,
            runs_offset,
            table_offset,
            name_bytes: name_bytes as usize,
            entries,
            observed,
            bits,
        })
    }

    pub fn observed_packs(&self) -> u32 {
        self.observed
    }

    pub fn pack_count(&self) -> usize {
        self.packs.len()
    }

    pub fn find(&self, name: &[u8]) -> Option<&[u8]> {
        if name.len() != self.name_bytes {
            return None;
        }
        let run = if self.bits != 0 {
            (be32(name) >> (32 - self.bits)) as usize
        } else {
            0
        };
        let mut low = le32(&self.index[self.runs_offset + 4 * run..]).min(self.entries);
        let mut high = le32(&self.index[self.runs_offset + 4 * (run + 1)..]).min(self.entries);
        let stride = self.name_bytes + ENTRY_TAIL_BYTES;
        while low < high {
            let mid = low + (high - low) / 2;
            let at = self.table_offset + stride * mid as usize;
            let entry_name = &self.index[at..at + self.name_bytes];
            match entry_name.cmp(name) {
                std::cmp::Ordering::Less => low = mid + 1,
                std::cmp::Ordering::Greater => high = mid,
                std::cmp::Ordering::Equal => {
                    let pack = le32(&self.index[at + self.name_bytes..]) as usize;
                    let offset = le32(&self.index[at + self.name_bytes + 4..]) as usize;
                    let length = le32(&self.index[at + self.name_bytes + 8..]) as usize;
                    let data = self.packs.get(pack)?.as_ref()?;
                    return data.get(offset..offset.checked_add(length)?);
                }
            }
        }
        None
    }
}
