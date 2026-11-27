// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, FileSource};
use switch_apfs_read::{Container, Entry, Fs, Step, Stream, XattrData, stream};
use tempfile::NamedTempFile;
use udif::{DmgReader, DmgReaderOptions};

use crate::metallib::{FAT_MAGIC, HEADER_LEN, MTLB_MAGIC};
use crate::{CrawlError, Result};

const KOLY_MAGIC: &[u8; 4] = b"koly";
const NXSB_MAGIC: &[u8; 4] = b"NXSB";
const GPT_MAGIC: &[u8; 8] = b"EFI PART";
const APFS_GPT_TYPE_GUID: [u8; 16] = [
    0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC,
];
const STREAM_CHUNK_BYTES: usize = 4 << 20;

pub struct OpenedImage {
    source: FileSource,
    _temp: Option<NamedTempFile>,
}

impl OpenedImage {
    pub fn open(path: &Path) -> Result<Self> {
        let mut file = File::open(path)?;
        let len = file.seek(SeekFrom::End(0))?;
        if has_magic_at(&mut file, len.saturating_sub(512), KOLY_MAGIC)? && len >= 512 {
            return open_udif(file);
        }
        if let Some(base) = find_apfs_base(&mut file, len)? {
            return Ok(Self {
                source: FileSource::open(path, base)?,
                _temp: None,
            });
        }
        Err(CrawlError::Invalid(format!(
            "{}: neither raw APFS, GPT-wrapped APFS, nor UDIF DMG",
            path.display()
        )))
    }

    pub fn source(&self) -> &(dyn BlockSource + Sync) {
        &self.source
    }
}

fn has_magic_at(file: &mut File, offset: u64, magic: &[u8]) -> Result<bool> {
    file.seek(SeekFrom::Start(offset))?;
    let mut buf = vec![0u8; magic.len()];
    match file.read_exact(&mut buf) {
        Ok(()) => Ok(buf == magic),
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => Ok(false),
        Err(e) => Err(e.into()),
    }
}

fn find_apfs_base(file: &mut File, len: u64) -> Result<Option<u64>> {
    if len >= 0x24 && has_magic_at(file, 0x20, NXSB_MAGIC)? {
        return Ok(Some(0));
    }
    for sector in [512u64, 4096u64] {
        if len < sector * 2 || !has_magic_at(file, sector, GPT_MAGIC)? {
            continue;
        }
        file.seek(SeekFrom::Start(sector))?;
        let mut hdr = [0u8; 92];
        file.read_exact(&mut hdr)?;
        let entries_lba = u64::from_le_bytes(hdr[72..80].try_into().expect("8 bytes"));
        let entry_count = u32::from_le_bytes(hdr[80..84].try_into().expect("4 bytes")) as usize;
        let entry_size = u32::from_le_bytes(hdr[84..88].try_into().expect("4 bytes")) as usize;
        if entry_size < 128 || entry_count > 1024 {
            continue;
        }
        let table_off = entries_lba.saturating_mul(sector);
        let mut entry = vec![0u8; entry_size];
        for idx in 0..entry_count {
            let off = table_off.saturating_add((idx * entry_size) as u64);
            if off.saturating_add(entry_size as u64) > len {
                break;
            }
            file.seek(SeekFrom::Start(off))?;
            file.read_exact(&mut entry)?;
            if entry[..16] == APFS_GPT_TYPE_GUID {
                let first_lba = u64::from_le_bytes(entry[32..40].try_into().expect("8 bytes"));
                let base = first_lba.saturating_mul(sector);
                if has_magic_at(file, base + 0x20, NXSB_MAGIC)? {
                    return Ok(Some(base));
                }
            }
        }
    }
    Ok(None)
}

fn open_udif(mut file: File) -> Result<OpenedImage> {
    file.seek(SeekFrom::Start(0))?;
    let mut dmg = DmgReader::with_options(
        file,
        DmgReaderOptions {
            verify_checksums: false,
        },
    )?;
    let apfs_parts: Vec<i32> = dmg
        .partitions()
        .iter()
        .filter(|p| p.name.contains("Apple_APFS"))
        .map(|p| p.id)
        .collect();
    if apfs_parts.len() != 1 {
        return Err(CrawlError::Invalid(format!(
            "UDIF image does not have a single Apple_APFS partition: {:?}",
            dmg.list_partitions()
        )));
    }
    let mut temp = NamedTempFile::new()?;
    dmg.decompress_partition_to(apfs_parts[0], temp.as_file_mut())?;
    temp.as_file_mut().flush()?;
    let len = temp.as_file_mut().seek(SeekFrom::End(0))?;
    let base = find_apfs_base(temp.as_file_mut(), len)?
        .ok_or_else(|| CrawlError::Invalid("unwrapped UDIF Apple_APFS partition lacks NXSB".into()))?;
    let source = FileSource::open(temp.path(), base)?;
    Ok(OpenedImage {
        source,
        _temp: Some(temp),
    })
}

#[derive(Clone, Debug)]
pub enum ApfsXattr {
    Embedded(Vec<u8>),
    Stream(Stream),
}

#[derive(Clone, Debug)]
pub enum ApfsFileData {
    Plain(Stream),
    Compressed {
        decmpfs: ApfsXattr,
        fork: Option<ApfsXattr>,
        uncompressed_size: u64,
    },
}

#[derive(Clone, Debug)]
pub struct ApfsFile {
    pub relative: String,
    pub size: u64,
    pub data: ApfsFileData,
}

pub fn walk_apfs_files(src: &(dyn BlockSource + Sync), skip_suffixes: &[&str], min_size: u64) -> Result<Vec<ApfsFile>> {
    walk_apfs_files_cancelable(src, skip_suffixes, min_size, &AtomicBool::new(false))
}

pub fn walk_apfs_files_cancelable(
    src: &(dyn BlockSource + Sync),
    skip_suffixes: &[&str],
    min_size: u64,
    cancel: &AtomicBool,
) -> Result<Vec<ApfsFile>> {
    let container = Container::open(src)?;
    if container.volumes.is_empty() {
        return Err(CrawlError::Invalid("APFS container has no volumes".into()));
    }
    let mut files = Vec::new();
    let mut cancelled = false;
    for volume in &container.volumes {
        if cancel.load(Ordering::Relaxed) {
            return Err(CrawlError::Cancelled);
        }
        let fs = Fs::open(src, volume)?;
        let walk_res = fs.walk("/", &mut |entry: &Entry| {
            if cancel.load(Ordering::Relaxed) {
                cancelled = true;
                return Err(switch_apfs_read::ReadError::invalid("cancelled"));
            }
            let relative = entry.path.trim_start_matches('/');
            let basename = relative.rsplit('/').next().unwrap_or(relative);
            if entry.inode.is_dir() {
                if !relative.is_empty()
                    && (skip_suffixes.iter().any(|s| basename.ends_with(s))
                        || crate::validate_portable_rel_path(relative).is_err())
                {
                    return Ok(Step::Over);
                }
                return Ok(Step::Into);
            }
            if !entry.inode.is_regular()
                || skip_suffixes.iter().any(|s| basename.ends_with(s))
                || crate::validate_portable_rel_path(relative).is_err()
            {
                return Ok(Step::Over);
            }
            if let Some(f) = extract_apfs_file(src, entry, relative, min_size)? {
                files.push(f);
            }
            Ok(Step::Over)
        });
        if cancelled {
            return Err(CrawlError::Cancelled);
        }
        walk_res?;
    }
    Ok(files)
}

fn xattr_of(entry: &Entry, name: &str) -> std::result::Result<Option<ApfsXattr>, switch_apfs_read::ReadError> {
    let Some((x, stream_opt)) = entry.xattr(name) else {
        return Ok(None);
    };
    match (&x.data, stream_opt) {
        (XattrData::Embedded(bytes), _) => Ok(Some(ApfsXattr::Embedded(bytes.clone()))),
        (XattrData::Stream { .. }, Some(s)) => Ok(Some(ApfsXattr::Stream(s.clone()))),
        (XattrData::Stream { id, .. }, None) => Err(switch_apfs_read::ReadError::invalid(format!(
            "{}: xattr {name} stream {id} missing extents",
            entry.path
        ))),
    }
}

fn extract_apfs_file(
    src: &dyn BlockSource,
    entry: &Entry,
    relative: &str,
    min_size: u64,
) -> std::result::Result<Option<ApfsFile>, switch_apfs_read::ReadError> {
    if entry.inode.is_compressed() {
        let Some(decmpfs) = xattr_of(entry, switch_decmpfs::XATTR)? else {
            return Ok(None);
        };
        let header_bytes = match &decmpfs {
            ApfsXattr::Embedded(b) => b.clone(),
            ApfsXattr::Stream(s) => read_stream_range_raw(src, s, 0, 16)?,
        };
        let Ok(hdr) = switch_decmpfs::Header::parse(&header_bytes) else {
            return Ok(None);
        };
        if hdr.size < min_size {
            return Ok(None);
        }
        let fork = xattr_of(entry, switch_decmpfs::RESOURCE_FORK)?;
        return Ok(Some(ApfsFile {
            relative: relative.to_owned(),
            size: hdr.size,
            data: ApfsFileData::Compressed {
                decmpfs,
                fork,
                uncompressed_size: hdr.size,
            },
        }));
    }
    let Some(s) = &entry.data else {
        return Ok(None);
    };
    if s.size < min_size {
        return Ok(None);
    }
    Ok(Some(ApfsFile {
        relative: relative.to_owned(),
        size: s.size,
        data: ApfsFileData::Plain(s.clone()),
    }))
}

pub fn read_stream_all(src: &dyn BlockSource, s: &Stream) -> Result<Vec<u8>> {
    let cap = usize::try_from(s.size).map_err(|_| CrawlError::Invalid(format!("stream {} too large", s.id)))?;
    let mut out = Vec::with_capacity(cap);
    stream::read(src, s, &mut |chunk| out.extend_from_slice(chunk))?;
    Ok(out)
}

fn read_stream_range_raw(
    src: &dyn BlockSource,
    s: &Stream,
    offset: u64,
    len: usize,
) -> std::result::Result<Vec<u8>, switch_apfs_read::ReadError> {
    let want_end = offset.saturating_add(len as u64).min(s.size);
    if offset >= want_end {
        return Ok(Vec::new());
    }
    let total = (want_end - offset) as usize;
    let mut out = vec![0u8; total];
    let mut block_buf = vec![0u8; BLOCK_SIZE];
    for ext in &s.extents {
        let ext_start = ext.logical_addr;
        let ext_end = ext.logical_addr.saturating_add(ext.len);
        let lo = offset.max(ext_start);
        let hi = want_end.min(ext_end);
        if lo >= hi || ext.phys_block_num == 0 {
            continue;
        }
        let mut cur = lo;
        while cur < hi {
            let rel_in_ext = cur - ext_start;
            let block_idx = rel_in_ext / BLOCK_SIZE as u64;
            let in_block = (rel_in_ext % BLOCK_SIZE as u64) as usize;
            let take = ((hi - cur) as usize).min(BLOCK_SIZE - in_block);
            src.read_blocks(ext.phys_block_num + block_idx, &mut block_buf)?;
            let dst_off = (cur - offset) as usize;
            out[dst_off..dst_off + take].copy_from_slice(&block_buf[in_block..in_block + take]);
            cur += take as u64;
        }
    }
    Ok(out)
}

pub fn read_stream_range(src: &dyn BlockSource, s: &Stream, offset: u64, len: usize) -> Result<Vec<u8>> {
    Ok(read_stream_range_raw(src, s, offset, len)?)
}

pub fn read_compressed(src: &dyn BlockSource, decmpfs: &ApfsXattr, fork: Option<&ApfsXattr>) -> Result<Vec<u8>> {
    let xattr_bytes = match decmpfs {
        ApfsXattr::Embedded(b) => b.clone(),
        ApfsXattr::Stream(s) => read_stream_all(src, s)?,
    };
    let fork_bytes = match fork {
        None => None,
        Some(ApfsXattr::Embedded(b)) => Some(b.clone()),
        Some(ApfsXattr::Stream(s)) => Some(read_stream_all(src, s)?),
    };
    Ok(switch_decmpfs::decompress(&xattr_bytes, fork_bytes.as_deref())?)
}

pub fn fat_has_mtlb_slice(src: &dyn BlockSource, s: &Stream) -> Result<bool> {
    let hdr = read_stream_range(src, s, 0, 8)?;
    if hdr.len() < 8 || &hdr[..4] != FAT_MAGIC {
        return Ok(false);
    }
    let count = u32::from_be_bytes(hdr[4..8].try_into().expect("4 bytes")) as usize;
    let max_count = s.size.saturating_sub(8) as usize / 20;
    let n = count.min(max_count);
    let table = read_stream_range(src, s, 8, n * 20)?;
    for i in 0..n {
        let base = i * 20;
        let offset = u32::from_be_bytes(table[base + 8..base + 12].try_into().expect("4 bytes")) as u64;
        let size = u32::from_be_bytes(table[base + 12..base + 16].try_into().expect("4 bytes")) as u64;
        if size >= HEADER_LEN as u64 && offset.saturating_add(size) <= s.size {
            let magic = read_stream_range(src, s, offset, 4)?;
            if magic.as_slice() == MTLB_MAGIC {
                return Ok(true);
            }
        }
    }
    Ok(false)
}

pub fn scan_plain_stream(
    src: &dyn BlockSource,
    s: &Stream,
    msl_needle: &[u8],
    on_mtlb: &mut dyn FnMut(u64, Vec<u8>) -> Result<()>,
) -> Result<usize> {
    let overlap = msl_needle.len().saturating_sub(1).max(3);
    let mut window: Vec<u8> = Vec::with_capacity(STREAM_CHUNK_BYTES + overlap);
    let mut window_start: u64 = 0;
    let mut mtlb_min_at: u64 = 0;
    let mut msl_min_at: u64 = 0;
    let mut msl_count: usize = 0;
    let mut scan_err: Option<CrawlError> = None;

    let mut process_window = |buf: &[u8], base: u64| -> Result<()> {
        let mut pos = 0usize;
        while let Some(rel) = find_subslice(&buf[pos..], MTLB_MAGIC) {
            let hit = pos + rel;
            let at = base + hit as u64;
            pos = hit + 4;
            if at < mtlb_min_at {
                continue;
            }
            if at + HEADER_LEN as u64 <= s.size {
                let hdr = if hit + HEADER_LEN <= buf.len() {
                    buf[hit..hit + HEADER_LEN].to_vec()
                } else {
                    read_stream_range(src, s, at, HEADER_LEN)?
                };
                let size = u64::from_le_bytes(hdr[0x10..0x18].try_into().expect("8 bytes"));
                let listing = u64::from_le_bytes(hdr[0x18..0x20].try_into().expect("8 bytes"));
                if size >= HEADER_LEN as u64 && size <= s.size - at && listing == HEADER_LEN as u64 {
                    let lib = if hit + size as usize <= buf.len() {
                        buf[hit..hit + size as usize].to_vec()
                    } else {
                        read_stream_range(src, s, at, size as usize)?
                    };
                    on_mtlb(at, lib)?;
                    mtlb_min_at = at + size;
                    let skip_end = (mtlb_min_at - base).min(buf.len() as u64) as usize;
                    pos = pos.max(skip_end);
                }
            }
        }
        let mut mpos = 0usize;
        while let Some(rel) = find_subslice(&buf[mpos..], msl_needle) {
            let hit = mpos + rel;
            let at = base + hit as u64;
            if at >= msl_min_at {
                msl_count += 1;
                msl_min_at = at + msl_needle.len() as u64;
                mpos = hit + msl_needle.len();
            } else {
                mpos = hit + 1;
            }
        }
        Ok(())
    };

    stream::read(src, s, &mut |chunk| {
        if scan_err.is_some() {
            return;
        }
        window.extend_from_slice(chunk);
        if window.len() >= STREAM_CHUNK_BYTES {
            if let Err(e) = process_window(&window, window_start) {
                scan_err = Some(e);
                return;
            }
            let keep = overlap.min(window.len());
            let drop = window.len() - keep;
            window.copy_within(drop.., 0);
            window.truncate(keep);
            window_start += drop as u64;
        }
    })?;
    if let Some(e) = scan_err {
        return Err(e);
    }
    if !window.is_empty() {
        process_window(&window, window_start)?;
    }
    Ok(msl_count)
}

pub(crate) fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    let first = needle[0];
    let limit = haystack.len() - needle.len();
    let mut i = 0;
    while i <= limit {
        let rel = haystack[i..=limit].iter().position(|&b| b == first)?;
        let pos = i + rel;
        if &haystack[pos..pos + needle.len()] == needle {
            return Some(pos);
        }
        i = pos + 1;
    }
    None
}
