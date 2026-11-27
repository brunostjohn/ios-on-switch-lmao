// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use switch_apfs_format::io::BlockSource;

use crate::apfs::{
    ApfsFile, ApfsFileData, OpenedImage, fat_has_mtlb_slice, find_subslice, read_compressed, read_stream_all,
    read_stream_range, scan_plain_stream, walk_apfs_files_cancelable,
};
use crate::metallib::{FAT_MAGIC, HEADER_LEN, MTLB_MAGIC, parse_functions, parse_functions_slice, slices, thin};
use crate::{CrawlError, Result, validate_portable_rel_path};

const SCAN_BATCH_SIZE: usize = 64;

const MACHO_MAGIC: &[u8; 4] = &[0xCF, 0xFA, 0xED, 0xFE];
const GPU_CPU_TYPE: u32 = 0x0100_0013;
const PIPELINE_LIBRARY_MAGIC: &[u8; 4] = b"LPPM";
const MSL_NEEDLE: &[u8] = b"#include <metal_stdlib>";

pub const SKIP_SUFFIXES: &[&str] = &[
    ".png", ".car", ".heic", ".jpg", ".mov", ".m4a", ".caf", ".ttf", ".ttc", ".otf", ".mp4", ".aar", ".lproj",
];

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SliceInfo {
    pub cpusubtype: u32,
    pub functions: usize,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LibraryEntry {
    pub r#where: String,
    pub sha256: String,
    pub bytes: usize,
    pub types: BTreeMap<String, usize>,
    pub air: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub slices: Option<Vec<SliceInfo>>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EmbeddedEntry {
    pub r#where: String,
    pub sha256: String,
    pub bytes: usize,
    pub types: BTreeMap<String, usize>,
    pub air: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub slices: Option<Vec<SliceInfo>>,
    pub container: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct UnreadableEntry {
    pub r#where: String,
    pub why: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CrawlInventory {
    pub files: usize,
    pub embedded: usize,
    pub embedded_seeded: usize,
    pub unreadable: Vec<UnreadableEntry>,
    pub msl_sources: BTreeMap<String, usize>,
    pub libraries: Vec<LibraryEntry>,
    pub embedded_libraries: Vec<EmbeddedEntry>,
}

struct FileScan {
    root_lib: Option<(String, Vec<u8>, LibraryEntry)>,
    embedded: Vec<(EmbeddedEntry, Option<Vec<u8>>)>,
    unreadable: Vec<UnreadableEntry>,
    msl: Option<(String, usize)>,
}

fn container_kind(head: &[u8]) -> &'static str {
    if head.get(..4) == Some(PIPELINE_LIBRARY_MAGIC.as_slice()) {
        return "pipeline-library";
    }
    if head.get(..4) == Some(MACHO_MAGIC.as_slice())
        && let Some(cpu_bytes) = head.get(4..8)
        && u32::from_le_bytes(cpu_bytes.try_into().expect("4 bytes")) == GPU_CPU_TYPE
    {
        return "gpu-archive";
    }
    "binary"
}

fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

fn build_library_entry(data: &[u8], where_str: String) -> LibraryEntry {
    let raw_slices = slices(data);
    let mut found: Vec<(u32, Vec<crate::metallib::Function>)> = Vec::with_capacity(raw_slices.len());
    let mut seen_subtypes: BTreeMap<u32, usize> = BTreeMap::new();
    for (sub, slice_bytes) in raw_slices {
        let funcs = parse_functions_slice(slice_bytes).unwrap_or_default();
        if let Some(&idx) = seen_subtypes.get(&sub) {
            found[idx].1 = funcs;
        } else {
            seen_subtypes.insert(sub, found.len());
            found.push((sub, funcs));
        }
    }
    let mut types: BTreeMap<String, usize> = BTreeMap::new();
    let mut air_set: BTreeSet<String> = BTreeSet::new();
    for (_, funcs) in &found {
        for f in funcs {
            *types.entry(f.kind.to_string()).or_insert(0) += 1;
            if !f.air.is_empty() {
                air_set.insert(f.air.clone());
            }
        }
    }
    let slices_meta = if found.len() > 1 {
        Some(
            found
                .iter()
                .map(|(sub, fs)| SliceInfo {
                    cpusubtype: *sub,
                    functions: fs.len(),
                })
                .collect(),
        )
    } else {
        None
    };
    LibraryEntry {
        r#where: where_str,
        sha256: sha256_hex(data),
        bytes: data.len(),
        types,
        air: air_set.into_iter().collect(),
        slices: slices_meta,
    }
}

fn make_embedded_entry(data: &[u8], where_str: String, container: &str) -> EmbeddedEntry {
    let base = build_library_entry(data, where_str);
    EmbeddedEntry {
        r#where: base.r#where,
        sha256: base.sha256,
        bytes: base.bytes,
        types: base.types,
        air: base.air,
        slices: base.slices,
        container: container.to_owned(),
    }
}

fn scan_bytes_file(relative: &str, data: Vec<u8>) -> FileScan {
    let mut result = FileScan {
        root_lib: None,
        embedded: Vec::new(),
        unreadable: Vec::new(),
        msl: None,
    };
    if data.len() < HEADER_LEN {
        return result;
    }
    let head = &data[..8];
    let is_mtlb_file = &head[..4] == MTLB_MAGIC || (&head[..4] == FAT_MAGIC && thin(&data).is_some());
    if is_mtlb_file {
        match parse_functions(&data) {
            None => result.unreadable.push(UnreadableEntry {
                r#where: relative.to_owned(),
                why: "function list does not parse".to_owned(),
            }),
            Some(funcs) if funcs.is_empty() => {}
            Some(_) => {
                let entry = build_library_entry(&data, relative.to_owned());
                result.root_lib = Some((relative.to_owned(), data, entry));
            }
        }
        return result;
    }
    let container = container_kind(head);
    let mut at = 0usize;
    while let Some(rel) = find_subslice(&data[at..], MTLB_MAGIC) {
        at += rel;
        if at + HEADER_LEN <= data.len() {
            let size = u64::from_le_bytes(data[at + 0x10..at + 0x18].try_into().expect("8 bytes"));
            let listing = u64::from_le_bytes(data[at + 0x18..at + 0x20].try_into().expect("8 bytes"));
            if size >= HEADER_LEN as u64 && size <= (data.len() - at) as u64 && listing == HEADER_LEN as u64 {
                let lib = &data[at..at + size as usize];
                let where_str = format!("{relative}@{at:#x}");
                if parse_functions(lib).is_none() {
                    result.unreadable.push(UnreadableEntry {
                        r#where: where_str,
                        why: "function list does not parse".to_owned(),
                    });
                } else {
                    let entry = make_embedded_entry(lib, where_str, container);
                    let bytes = (container == "binary").then(|| lib.to_vec());
                    result.embedded.push((entry, bytes));
                }
                at += size as usize;
                continue;
            }
        }
        at += 4;
    }
    let mut msl_count = 0usize;
    let mut mpos = 0usize;
    while let Some(rel) = find_subslice(&data[mpos..], MSL_NEEDLE) {
        msl_count += 1;
        mpos += rel + MSL_NEEDLE.len();
    }
    if msl_count > 0 {
        result.msl = Some((relative.to_owned(), msl_count));
    }
    result
}

fn scan_apfs_file(src: &(dyn BlockSource + Sync), file: &ApfsFile) -> Result<FileScan> {
    match &file.data {
        ApfsFileData::Compressed { decmpfs, fork, .. } => {
            let data = read_compressed(src, decmpfs, fork.as_ref())?;
            Ok(scan_bytes_file(&file.relative, data))
        }
        ApfsFileData::Plain(s) => {
            let head = read_stream_range(src, s, 0, 8)?;
            if head.len() < 8 {
                return Ok(FileScan {
                    root_lib: None,
                    embedded: Vec::new(),
                    unreadable: Vec::new(),
                    msl: None,
                });
            }
            let is_mtlb_file = &head[..4] == MTLB_MAGIC || (&head[..4] == FAT_MAGIC && fat_has_mtlb_slice(src, s)?);
            if is_mtlb_file {
                let data = read_stream_all(src, s)?;
                return Ok(scan_bytes_file(&file.relative, data));
            }
            let container = container_kind(&head);
            let mut result = FileScan {
                root_lib: None,
                embedded: Vec::new(),
                unreadable: Vec::new(),
                msl: None,
            };
            let msl_count = scan_plain_stream(src, s, MSL_NEEDLE, &mut |at, lib| {
                let where_str = format!("{}@{at:#x}", file.relative);
                if parse_functions(&lib).is_none() {
                    result.unreadable.push(UnreadableEntry {
                        r#where: where_str,
                        why: "function list does not parse".to_owned(),
                    });
                } else {
                    let entry = make_embedded_entry(&lib, where_str, container);
                    let bytes = (container == "binary").then_some(lib);
                    result.embedded.push((entry, bytes));
                }
                Ok(())
            })?;
            if msl_count > 0 {
                result.msl = Some((file.relative.clone(), msl_count));
            }
            Ok(result)
        }
    }
}

pub struct Crawler {
    out: PathBuf,
    files: BTreeMap<String, LibraryEntry>,
    seen_lower: BTreeMap<String, String>,
    embedded: BTreeMap<String, EmbeddedEntry>,
    unreadable: Vec<UnreadableEntry>,
    sources: BTreeMap<String, usize>,
}

impl Crawler {
    pub fn new(out: impl AsRef<Path>) -> Result<Self> {
        let out = out.as_ref().to_path_buf();
        if out.exists() {
            fs::remove_dir_all(&out)?;
        }
        fs::create_dir_all(out.join("root"))?;
        fs::create_dir_all(out.join("data"))?;
        Ok(Self {
            out,
            files: BTreeMap::new(),
            seen_lower: BTreeMap::new(),
            embedded: BTreeMap::new(),
            unreadable: Vec::new(),
            sources: BTreeMap::new(),
        })
    }

    pub fn crawl_path(&mut self, path: &Path) -> Result<()> {
        if path.is_dir() {
            self.crawl_directory(path)
        } else {
            self.crawl_image(path)
        }
    }

    pub fn crawl_image(&mut self, image_path: &Path) -> Result<()> {
        self.crawl_image_cancelable(image_path, &AtomicBool::new(false))
    }

    pub fn crawl_image_cancelable(&mut self, image_path: &Path, cancel: &AtomicBool) -> Result<()> {
        if cancel.load(Ordering::Relaxed) {
            return Err(CrawlError::Cancelled);
        }
        let opened = OpenedImage::open(image_path)?;
        let src = opened.source();
        let files = walk_apfs_files_cancelable(src, SKIP_SUFFIXES, HEADER_LEN as u64, cancel)?;
        for chunk in files.chunks(SCAN_BATCH_SIZE) {
            if cancel.load(Ordering::Relaxed) {
                return Err(CrawlError::Cancelled);
            }
            let scans: Vec<FileScan> = chunk
                .par_iter()
                .map(|f| scan_apfs_file(src, f))
                .collect::<Result<Vec<_>>>()?;
            for scan in scans {
                self.apply_scan(scan)?;
            }
        }
        Ok(())
    }

    pub fn crawl_directory(&mut self, root: &Path) -> Result<()> {
        let mut entries = Vec::new();
        collect_host_files(root, root, &mut entries)?;
        for chunk in entries.chunks(SCAN_BATCH_SIZE) {
            let scans: Vec<FileScan> = chunk
                .par_iter()
                .map(|(path, rel)| {
                    let data = fs::read(path)?;
                    Ok(scan_bytes_file(rel, data))
                })
                .collect::<Result<Vec<_>>>()?;
            for scan in scans {
                self.apply_scan(scan)?;
            }
        }
        Ok(())
    }

    fn apply_scan(&mut self, scan: FileScan) -> Result<()> {
        if let Some((relative, data, entry)) = scan.root_lib {
            validate_portable_rel_path(&relative)?;
            let lower = relative.to_ascii_lowercase();
            if let Some(prev) = self.seen_lower.get(&lower)
                && prev != &relative
            {
                return Err(CrawlError::Invalid(format!(
                    "case-colliding crawl library paths {prev:?} and {relative:?}"
                )));
            }
            self.seen_lower.insert(lower, relative.clone());
            let target = self.out.join("root").join(&relative);
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            fs::write(&target, &data)?;
            self.files.insert(relative, entry);
        }
        for (entry, bytes_opt) in scan.embedded {
            let sha = entry.sha256.clone();
            if sha.len() != 64 || !sha.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')) {
                return Err(CrawlError::Invalid(format!("invalid embedded sha256 {sha:?}")));
            }
            let is_binary = entry.container == "binary";
            self.embedded.entry(sha.clone()).or_insert(entry);
            if is_binary && let Some(bytes) = bytes_opt {
                fs::write(self.out.join("data").join(format!("{sha}.metallib")), &bytes)?;
            }
        }
        self.unreadable.extend(scan.unreadable);
        if let Some((relative, count)) = scan.msl {
            *self.sources.entry(relative).or_insert(0) += count;
        }
        Ok(())
    }

    pub fn inventory(&self) -> CrawlInventory {
        let embedded_seeded = self.embedded.values().filter(|e| e.container == "binary").count();
        let mut libraries: Vec<LibraryEntry> = self.files.values().cloned().collect();
        libraries.sort_by(|a, b| a.r#where.cmp(&b.r#where));
        let mut embedded_libraries: Vec<EmbeddedEntry> = self.embedded.values().cloned().collect();
        embedded_libraries.sort_by(|a, b| a.r#where.cmp(&b.r#where));
        CrawlInventory {
            files: self.files.len(),
            embedded: self.embedded.len(),
            embedded_seeded,
            unreadable: self.unreadable.clone(),
            msl_sources: self.sources.clone(),
            libraries,
            embedded_libraries,
        }
    }
}

fn collect_host_files(root: &Path, dir: &Path, out: &mut Vec<(PathBuf, String)>) -> Result<()> {
    let mut children: Vec<_> = fs::read_dir(dir)?.collect::<std::io::Result<Vec<_>>>()?;
    children.sort_by_key(|e| e.file_name());
    for entry in children {
        let path = entry.path();
        let meta = fs::symlink_metadata(&path)?;
        if meta.file_type().is_symlink() {
            continue;
        }
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if SKIP_SUFFIXES.iter().any(|s| name_str.ends_with(s)) {
            continue;
        }
        if meta.is_dir() {
            collect_host_files(root, &path, out)?;
        } else if meta.is_file() && meta.len() >= HEADER_LEN as u64 {
            let rel = path
                .strip_prefix(root)
                .map_err(|e| crate::CrawlError::Invalid(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            out.push((path, rel));
        }
    }
    Ok(())
}

pub fn format_inventory_json(inv: &CrawlInventory) -> Result<String> {
    let mut buf = Vec::new();
    let formatter = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, formatter);
    inv.serialize(&mut ser)?;
    buf.push(b'\n');
    String::from_utf8(buf).map_err(|e| crate::CrawlError::Invalid(e.to_string()))
}
