// SPDX-License-Identifier: GPL-3.0-or-later
//! Binary encoding and decoding of `Switch/pretranslated/.index` (`SWOV` v2) and `.originals` (`SWOR` v1), matching
//! `tools/install/overlay_index.py` and `kits/SwitchPretranslated/`.

use std::collections::{BTreeMap, HashMap, HashSet};

pub const MAGIC: &[u8; 4] = b"SWOV";
pub const VERSION: u32 = 2;
pub const NAME: &str = ".index";
pub const ORIGINALS_MAGIC: &[u8; 4] = b"SWOR";
pub const ORIGINALS_VERSION: u32 = 1;
pub const ORIGINALS_NAME: &str = ".originals";
pub const CDHASH_LEN: usize = 20;
pub const HEADER_SIZE: usize = 24;
pub const FILE_RECORD_SIZE: usize = 44;
pub const ORIGINALS_HEADER_SIZE: usize = 16;
pub const ORIGINALS_PAIR_SIZE: usize = 2 * CDHASH_LEN;
pub const MAX_PATH: usize = 1024;
pub const MAX_FILES: usize = 1 << 20;
pub const MAX_DIRECTORIES: usize = 1 << 20;
pub const MAX_ORIGINALS: usize = 1 << 16;
pub const MAX_INDEX_BYTES: usize = 64 << 20;

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum IndexError {
    #[error("{0:?} is not a path the overlay index takes")]
    BadPath(String),
    #[error("the overlay index lists a path twice: {0:?}")]
    DuplicatePath(String),
    #[error("a CDHash that is not {CDHASH_LEN} bytes")]
    BadCdhashLength,
    #[error("a file id of 0")]
    ZeroId,
    #[error("hard links of one System file with different Data files or CDHashes")]
    ConflictingHardLink,
    #[error("the overlay index names a Data file id twice")]
    DuplicateDataId,
    #[error("the overlay index names a directory id twice")]
    DuplicateDirectoryId,
    #[error("the overlay index is too large")]
    TooLarge,
    #[error("short overlay index")]
    Short,
    #[error("not a version {VERSION} overlay index")]
    BadHeader,
    #[error("the overlay index is not the size its header says")]
    BadSize,
    #[error("the overlay index strings are not packed in order")]
    BadStrings,
    #[error("the overlay index is not sorted, unique and packed")]
    UnsortedOrLoose,
    #[error("{0} is translated two ways")]
    ConflictingOriginal(String),
    #[error("too many originals")]
    TooManyOriginals,
    #[error("not a version {ORIGINALS_VERSION} originals table")]
    BadOriginalsHeader,
    #[error("the originals table is not the size its header says")]
    BadOriginalsSize,
    #[error("the originals table is not sorted and unique")]
    UnsortedOriginals,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IndexEntry {
    pub path: String,
    pub system_id: u64,
    pub data_id: u64,
    pub cdhash: [u8; CDHASH_LEN],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OriginalsPair {
    pub original: [u8; CDHASH_LEN],
    pub translated: [u8; CDHASH_LEN],
}

pub fn cdhash_from_slice(bytes: &[u8]) -> Result<[u8; CDHASH_LEN], IndexError> {
    bytes.try_into().map_err(|_| IndexError::BadCdhashLength)
}

pub fn check_path(path: &str) -> Result<(), IndexError> {
    if !path.starts_with('/')
        || path.ends_with('/')
        || path.contains("//")
        || path.split('/').any(|part| part == "." || part == "..")
        || path.contains('\0')
        || path.len() >= MAX_PATH
        || path == "/.index"
    {
        return Err(IndexError::BadPath(path.into()));
    }
    Ok(())
}

fn check_ids(files: &[IndexEntry], directories: &[u64]) -> Result<(), IndexError> {
    if files.iter().any(|f| f.system_id == 0 || f.data_id == 0) || directories.contains(&0) {
        return Err(IndexError::ZeroId);
    }
    let mut by_system: HashMap<u64, (u64, [u8; CDHASH_LEN])> = HashMap::with_capacity(files.len());
    let mut by_data: HashMap<u64, u64> = HashMap::with_capacity(files.len());
    for f in files {
        match by_system.get(&f.system_id) {
            Some(&(data_id, cdhash)) if (data_id, cdhash) != (f.data_id, f.cdhash) => {
                return Err(IndexError::ConflictingHardLink);
            }
            None => {
                by_system.insert(f.system_id, (f.data_id, f.cdhash));
            }
            Some(_) => {}
        }
        match by_data.get(&f.data_id) {
            Some(&system_id) if system_id != f.system_id => {
                return Err(IndexError::DuplicateDataId);
            }
            None => {
                by_data.insert(f.data_id, f.system_id);
            }
            Some(_) => {}
        }
    }
    let mut seen_dirs = HashSet::with_capacity(directories.len());
    for &d in directories {
        if !seen_dirs.insert(d) || by_data.contains_key(&d) {
            return Err(IndexError::DuplicateDirectoryId);
        }
    }
    Ok(())
}

pub fn encode(files: &[IndexEntry], directories: &[u64]) -> Result<Vec<u8>, IndexError> {
    let mut sorted: Vec<IndexEntry> = files.to_vec();
    sorted.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    for pair in sorted.windows(2) {
        if pair[0].path.as_bytes() == pair[1].path.as_bytes() {
            return Err(IndexError::DuplicatePath(pair[0].path.clone()));
        }
    }
    for entry in &sorted {
        check_path(&entry.path)?;
    }
    let mut dirs = directories.to_vec();
    dirs.sort_unstable();
    check_ids(&sorted, &dirs)?;
    if sorted.len() > MAX_FILES || dirs.len() > MAX_DIRECTORIES {
        return Err(IndexError::TooLarge);
    }

    let string_bytes: usize = sorted.iter().map(|e| e.path.len() + 1).sum();
    let string_u32 = u32::try_from(string_bytes).map_err(|_| IndexError::TooLarge)?;
    let total = HEADER_SIZE + FILE_RECORD_SIZE * sorted.len() + 8 * dirs.len() + string_bytes;
    if total > MAX_INDEX_BYTES {
        return Err(IndexError::TooLarge);
    }
    let mut out = Vec::with_capacity(total);

    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&(sorted.len() as u32).to_le_bytes());
    out.extend_from_slice(&(dirs.len() as u32).to_le_bytes());
    out.extend_from_slice(&string_u32.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());

    let mut offset = 0u32;
    for entry in &sorted {
        out.extend_from_slice(&entry.system_id.to_le_bytes());
        out.extend_from_slice(&entry.data_id.to_le_bytes());
        out.extend_from_slice(&offset.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&entry.cdhash);
        offset += entry.path.len() as u32 + 1;
    }
    for d in &dirs {
        out.extend_from_slice(&d.to_le_bytes());
    }
    for entry in &sorted {
        out.extend_from_slice(entry.path.as_bytes());
        out.push(0);
    }
    Ok(out)
}

pub fn decode(raw: &[u8]) -> Result<(Vec<IndexEntry>, Vec<u64>), IndexError> {
    if raw.len() < HEADER_SIZE {
        return Err(IndexError::Short);
    }
    let magic = &raw[0..4];
    let version = read_u32(raw, 4);
    let count = read_u32(raw, 8) as usize;
    let dirs_count = read_u32(raw, 12) as usize;
    let string_size = read_u32(raw, 16) as usize;
    let reserved = read_u32(raw, 20);
    if magic != MAGIC
        || version != VERSION
        || reserved != 0
        || count > MAX_FILES
        || dirs_count > MAX_DIRECTORIES
        || raw.len() > MAX_INDEX_BYTES
    {
        return Err(IndexError::BadHeader);
    }
    let expected = (HEADER_SIZE as u64)
        .checked_add((FILE_RECORD_SIZE as u64) * count as u64)
        .and_then(|v| v.checked_add(8 * dirs_count as u64))
        .and_then(|v| v.checked_add(string_size as u64))
        .ok_or(IndexError::BadSize)?;
    if raw.len() as u64 != expected {
        return Err(IndexError::BadSize);
    }

    let dirs_at = HEADER_SIZE + FILE_RECORD_SIZE * count;
    let strings_at = dirs_at + 8 * dirs_count;
    let strings = &raw[strings_at..];

    let mut files = Vec::with_capacity(count);
    let mut end = 0usize;
    for i in 0..count {
        let base = HEADER_SIZE + FILE_RECORD_SIZE * i;
        let system_id = read_u64(raw, base);
        let data_id = read_u64(raw, base + 8);
        let offset = read_u32(raw, base + 16) as usize;
        let pad = read_u32(raw, base + 20);
        let cdhash: [u8; CDHASH_LEN] = raw[base + 24..base + 44].try_into().expect("20-byte slice");
        if pad != 0 || offset != end {
            return Err(IndexError::BadStrings);
        }
        let rel_nul = strings[offset..]
            .iter()
            .position(|&b| b == 0)
            .ok_or(IndexError::BadStrings)?;
        let path = std::str::from_utf8(&strings[offset..offset + rel_nul])
            .map_err(|_| IndexError::BadStrings)?
            .to_owned();
        end = offset + rel_nul + 1;
        files.push(IndexEntry {
            path,
            system_id,
            data_id,
            cdhash,
        });
    }

    let mut directories = Vec::with_capacity(dirs_count);
    for i in 0..dirs_count {
        directories.push(read_u64(raw, dirs_at + 8 * i));
    }

    if end != string_size
        || files.windows(2).any(|w| w[0].path.as_bytes() >= w[1].path.as_bytes())
        || directories.windows(2).any(|w| w[0] >= w[1])
    {
        return Err(IndexError::UnsortedOrLoose);
    }
    for entry in &files {
        check_path(&entry.path)?;
    }
    check_ids(&files, &directories)?;
    Ok((files, directories))
}

pub fn encode_originals(pairs: &[OriginalsPair]) -> Result<Vec<u8>, IndexError> {
    let mut by_original: BTreeMap<[u8; CDHASH_LEN], [u8; CDHASH_LEN]> = BTreeMap::new();
    for pair in pairs {
        match by_original.get(&pair.original) {
            Some(&existing) if existing != pair.translated => {
                return Err(IndexError::ConflictingOriginal(hex::encode(pair.original)));
            }
            None => {
                by_original.insert(pair.original, pair.translated);
            }
            Some(_) => {}
        }
    }
    if by_original.len() > MAX_ORIGINALS {
        return Err(IndexError::TooManyOriginals);
    }
    let mut out = Vec::with_capacity(ORIGINALS_HEADER_SIZE + ORIGINALS_PAIR_SIZE * by_original.len());
    out.extend_from_slice(ORIGINALS_MAGIC);
    out.extend_from_slice(&ORIGINALS_VERSION.to_le_bytes());
    out.extend_from_slice(&(by_original.len() as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    for (original, translated) in by_original {
        out.extend_from_slice(&original);
        out.extend_from_slice(&translated);
    }
    Ok(out)
}

pub fn decode_originals(raw: &[u8]) -> Result<Vec<OriginalsPair>, IndexError> {
    if raw.len() < ORIGINALS_HEADER_SIZE {
        return Err(IndexError::Short);
    }
    let magic = &raw[0..4];
    let version = read_u32(raw, 4);
    let count = read_u32(raw, 8) as usize;
    let reserved = read_u32(raw, 12);
    if magic != ORIGINALS_MAGIC || version != ORIGINALS_VERSION || reserved != 0 || count > MAX_ORIGINALS {
        return Err(IndexError::BadOriginalsHeader);
    }
    if raw.len() != ORIGINALS_HEADER_SIZE + ORIGINALS_PAIR_SIZE * count {
        return Err(IndexError::BadOriginalsSize);
    }
    let mut out: Vec<OriginalsPair> = Vec::with_capacity(count);
    for i in 0..count {
        let base = ORIGINALS_HEADER_SIZE + ORIGINALS_PAIR_SIZE * i;
        let original: [u8; CDHASH_LEN] = raw[base..base + CDHASH_LEN].try_into().expect("20 bytes");
        let translated: [u8; CDHASH_LEN] = raw[base + CDHASH_LEN..base + ORIGINALS_PAIR_SIZE]
            .try_into()
            .expect("20 bytes");
        if let Some(prev) = out.last()
            && prev.original >= original
        {
            return Err(IndexError::UnsortedOriginals);
        }
        out.push(OriginalsPair { original, translated });
    }
    Ok(out)
}

fn read_u32(raw: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(raw[at..at + 4].try_into().expect("4 bytes"))
}

fn read_u64(raw: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(raw[at..at + 8].try_into().expect("8 bytes"))
}
