// SPDX-License-Identifier: GPL-3.0-or-later
//! Self-contained, IPSW-free shader seed reproducibility smoke check.
//!
//! Compiles only the metallibs bundled inside a `seed-<build>.tar` asset (`own.metallib`, `SwitchHWTest` app
//! libraries, and embedded driver libraries) through the full desktop directpack, directlink, native-seed, and
//! native-pack pipeline so CI can verify byte-identical GPU output across macOS arm64, Linux x86_64, and Windows
//! x86_64 without Apple IPSW files.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use serde_json::Value;
use switch_seed_link::records::read_records;
use switch_seed_link::{LinkConfig, link_buckets};
use switch_seed_plan::{pack_native_seed, restamp_buckets, split_native_lines};

use crate::asset::{UnpackedSeedAsset, parse_stack_key_hex, read_seed_asset_manifest, unpack_seed_asset};
use crate::error::{Result, SeedError};
use crate::ffi::{DirectpackArgs, NativeBucketArgs, alloc_scope, directpack, init_runtime, native_bucket};
use crate::fsutil::{collect_sorted_files, sha256_file};
use crate::pipeline::build_thread_pool;

const SMOKE_CACHE_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SeedSmokeEntry {
    pub rel_path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug)]
pub struct SeedSmokeReport {
    pub build: String,
    pub stack_key: String,
    pub corpus_lines: usize,
    pub pack_states: usize,
    pub pack_kernels: usize,
    pub entries: Vec<SeedSmokeEntry>,
}

impl SeedSmokeReport {
    pub fn format_manifest(&self) -> String {
        let mut out = format!(
            "# build={} stack={} corpus_lines={} pack_states={} pack_kernels={}\n",
            self.build, self.stack_key, self.corpus_lines, self.pack_states, self.pack_kernels
        );
        for entry in &self.entries {
            out.push_str(&format!("{}  {}  {}\n", entry.sha256, entry.size, entry.rel_path));
        }
        out
    }
}

pub fn run_seed_repro_smoke(seed_tar: &Path, work_dir: &Path, threads: Option<usize>) -> Result<SeedSmokeReport> {
    let manifest = read_seed_asset_manifest(seed_tar)?;
    let _cache_guard = init_runtime(SMOKE_CACHE_BYTES);
    fs::create_dir_all(work_dir).map_err(|e| SeedError::io_path(work_dir, e))?;

    let unpack_dir = work_dir.join("asset");
    let asset = unpack_seed_asset(
        seed_tar,
        &manifest.build,
        &manifest.quartzcore_metallib_sha256,
        None,
        &unpack_dir,
    )?;
    let stack_u64 = parse_stack_key_hex(&asset.manifest.stack_key)?;

    let crawl_dir = work_dir.join("crawl");
    let (allowed_libs, lib_root, data_libs) = stage_ipsw_free_libraries(&asset, &crawl_dir)?;
    let corpus_text = fs::read_to_string(&asset.corpus_log).map_err(|e| SeedError::io_path(&asset.corpus_log, e))?;
    let filtered = filter_ipsw_free_corpus(&corpus_text, &allowed_libs);
    if filtered.is_empty() {
        return Err(SeedError::Asset(format!(
            "{}: seed corpus has no IPSW-free lines matching bundled app/embedded libraries",
            asset.manifest.build
        )));
    }

    let smoke_corpus = work_dir.join("smoke_corpus.log");
    write_lines(&smoke_corpus, &filtered)?;

    let pool = build_thread_pool(threads)?;
    let scope = alloc_scope();
    let cancel = AtomicBool::new(false);

    let bucket_dir = work_dir.join("pack_bucket0");
    fs::create_dir_all(&bucket_dir).map_err(|e| SeedError::io_path(&bucket_dir, e))?;
    let records_path = bucket_dir.join("records.bin");
    pool.install(|| {
        directpack(&DirectpackArgs {
            kind: &asset.manifest.kind,
            qc_metallib: &asset.own_metallib,
            corpus_path: &smoke_corpus,
            output_path: &records_path,
            own_lib: Some(&asset.own_metallib),
            lib_root: Some(&lib_root),
            data_libs: Some(&data_libs),
            label_prefix: None,
            scope,
            cancel: &cancel,
        })
    })?;

    let parsed = read_records(&records_path)?;
    let pack_states = parsed.states.iter().filter(|s| s.outcome == 0).count();
    let pack_kernels = parsed.kernels.iter().filter(|k| k.outcome == 0).count();
    if pack_states == 0 || pack_kernels == 0 {
        return Err(SeedError::Asset(format!(
            "smoke directpack produced insufficient states ({pack_states}) or kernels ({pack_kernels})"
        )));
    }

    let pack_out_dir = work_dir.join("pack_out");
    fs::create_dir_all(&pack_out_dir).map_err(|e| SeedError::io_path(&pack_out_dir, e))?;
    link_buckets(&LinkConfig {
        buckets: vec![bucket_dir],
        out_dir: pack_out_dir.clone(),
        context_log: Some(asset.context_log.clone()),
        draw_context: Some(asset.draw_context.clone()),
        archives: None,
        corpus_report: None,
        stack: Some(stack_u64),
        write_debug_outputs: false,
    })?;
    let pack_bin = pack_out_dir.join("pack.bin");

    let native_bucket_dir = work_dir.join("native_bucket0");
    fs::create_dir_all(&native_bucket_dir).map_err(|e| SeedError::io_path(&native_bucket_dir, e))?;
    let (observed, derived) = split_native_lines(&filtered.join("\n"));
    let obs_path = native_bucket_dir.join("observed.log");
    let der_path = native_bucket_dir.join("derived.log");
    write_lines(&obs_path, &observed)?;
    write_lines(&der_path, &derived)?;
    pool.install(|| {
        native_bucket(&NativeBucketArgs {
            kind: &asset.manifest.kind,
            libs_store: &crawl_dir,
            qc_metallib: &asset.own_metallib,
            stack_key: 0,
            output_dir: &native_bucket_dir,
            corpora: &[obs_path.as_path()],
            derived: &[der_path.as_path()],
            scope,
            cancel: &cancel,
        })
    })?;
    let manifest_path = native_bucket_dir.join("manifest.json");
    let mut bucket_manifest: Value = serde_json::from_str(&fs::read_to_string(&manifest_path)?)?;
    if let Some(records) = bucket_manifest.get_mut("records").and_then(Value::as_array_mut) {
        for row in records {
            if let Some(obj) = row.as_object_mut() {
                obj.insert("source".to_owned(), Value::String("bucket-000.log".to_owned()));
            }
        }
    }
    fs::write(&manifest_path, serde_json::to_string(&bucket_manifest)? + "\n")
        .map_err(|e| SeedError::io_path(&manifest_path, e))?;

    let stamped_dir = work_dir.join("native_stamped");
    let native_out = work_dir.join("native_out");
    restamp_buckets(&asset.manifest.stack_key, &stamped_dir, &[native_bucket_dir])?;
    pack_native_seed(&stamped_dir, &native_out)?;

    let mut by_path: BTreeMap<String, (u64, String)> = BTreeMap::new();
    let (rec_sz, rec_sha) = sha256_file(&records_path)?;
    by_path.insert("records.bin".to_owned(), (rec_sz, hex::encode(rec_sha)));
    let (pack_sz, pack_sha) = sha256_file(&pack_bin)?;
    by_path.insert("pack.bin".to_owned(), (pack_sz, hex::encode(pack_sha)));

    let rec_dir_name = format!("rec-{}", asset.manifest.stack_key);
    let rec_dir = native_out.join(&rec_dir_name);
    let mut native_files = Vec::new();
    collect_sorted_files(&rec_dir, &mut native_files)?;
    if !rec_dir.join("index").is_file() {
        return Err(SeedError::Asset(format!(
            "smoke native seed missing {rec_dir_name}/index"
        )));
    }
    for path in native_files {
        let rel = path
            .strip_prefix(&native_out)
            .map_err(|e| SeedError::Asset(e.to_string()))?
            .to_string_lossy()
            .replace('\\', "/");
        let (sz, sha) = sha256_file(&path)?;
        by_path.insert(format!("native/{rel}"), (sz, hex::encode(sha)));
    }

    let entries = by_path
        .into_iter()
        .map(|(rel_path, (size, sha256))| SeedSmokeEntry { rel_path, size, sha256 })
        .collect();
    Ok(SeedSmokeReport {
        build: asset.manifest.build,
        stack_key: asset.manifest.stack_key,
        corpus_lines: filtered.len(),
        pack_states,
        pack_kernels,
        entries,
    })
}

fn stage_ipsw_free_libraries(
    asset: &UnpackedSeedAsset,
    crawl_dir: &Path,
) -> Result<(BTreeSet<String>, PathBuf, PathBuf)> {
    let lib_root = crawl_dir.join("root");
    let data_libs = crawl_dir.join("data");
    fs::create_dir_all(&lib_root).map_err(|e| SeedError::io_path(&lib_root, e))?;
    fs::create_dir_all(&data_libs).map_err(|e| SeedError::io_path(&data_libs, e))?;

    let mut allowed = BTreeSet::new();
    for (guest_path, src) in &asset.app_libraries {
        let dest = lib_root.join(guest_path);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
        }
        fs::copy(src, &dest).map_err(|e| SeedError::io_path(&dest, e))?;
        allowed.insert(format!("/{guest_path}"));
    }
    for emb in &asset.embedded_files {
        let name = emb
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or_else(|| SeedError::Asset("embedded file missing UTF-8 name".into()))?;
        fs::copy(emb, data_libs.join(name)).map_err(|e| SeedError::io_path(emb, e))?;
        if let Some(sha) = name.strip_suffix(".metallib") {
            allowed.insert(format!("data:{sha}"));
        }
    }
    Ok((allowed, lib_root, data_libs))
}

fn is_ipsw_free_corpus_line(line: &str, allowed_libs: &BTreeSet<String>) -> bool {
    if !(line.starts_with("R ok=1 ") || line.starts_with("C ok=1 ") || line.starts_with("D ok=1 ")) {
        return false;
    }
    let mut stage_count = 0usize;
    for section in line.split(" | ").skip(1) {
        let trimmed = section.trim();
        let is_stage = matches!(trimmed.split_whitespace().next(), Some("v" | "f" | "k" | "o" | "m"));
        if !is_stage {
            continue;
        }
        stage_count += 1;
        let lib = trimmed.split_whitespace().find_map(|tok| tok.strip_prefix("lib="));
        match lib {
            Some(path) if allowed_libs.contains(path) => {}
            _ => return false,
        }
    }
    stage_count > 0
}

fn filter_ipsw_free_corpus(text: &str, allowed_libs: &BTreeSet<String>) -> Vec<String> {
    text.lines()
        .filter(|line| is_ipsw_free_corpus_line(line, allowed_libs))
        .map(str::to_owned)
        .collect()
}

fn write_lines(path: &Path, lines: &[String]) -> Result<()> {
    let mut text = String::new();
    for line in lines {
        text.push_str(line);
        text.push('\n');
    }
    fs::write(path, text).map_err(|e| SeedError::io_path(path, e))
}
