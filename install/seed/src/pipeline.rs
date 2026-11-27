// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use switch_seed_crawl::{CrawlError, Crawler};
use switch_seed_plan::merge::PACK_REL_DIR;
use switch_seed_plan::plan::MAX_PLAN_BUCKETS;
use switch_seed_plan::{CACHE_ENTRY_FORMAT, Depth, staged_in_pack};

use crate::asset::{UnpackedSeedAsset, parse_stack_key_hex, unpack_seed_asset};
use crate::error::{Result, SeedError};
use crate::ffi::{alloc_scope, desktop_compiler_abi, init_runtime};
use crate::fsutil::{
    CacheLock, FileDigest, check_cancel, collect_inventory, collect_sorted_files, replace_dir_atomic, sha256_file,
    unique_temp_sibling, verify_inventory,
};
use crate::stages::{build_native_seed, build_pipeline_pack};

pub const QC_REL_PATH: &str = "System/Library/Frameworks/QuartzCore.framework/default.metallib";
const WORKER_STACK_BYTES: usize = 16 * 1024 * 1024;
const DEFAULT_TRANSLATION_CACHE_BYTES: u64 = 256 * 1024 * 1024;
const DEFAULT_NATIVE_BUCKETS: usize = 256;
const CRAWL_SCHEMA_VERSION: u32 = 3;
const RUN_SCHEMA_VERSION: u32 = 2;

#[derive(Clone, Debug)]
pub struct SeedInputs<'a> {
    pub build: &'a str,
    pub qc_sha256: &'a str,
    pub seed_tar: &'a Path,
    pub seed_tar_sha256: &'a str,
    pub os_container: &'a Path,
    pub system_cryptex: &'a Path,
    pub app_cryptex: &'a Path,
    pub os_sha256: Option<&'a str>,
    pub system_cryptex_sha256: Option<&'a str>,
    pub app_cryptex_sha256: Option<&'a str>,
    pub bundle_stack_key: Option<&'a str>,
    pub work_dir: &'a Path,
}

#[derive(Clone, Debug)]
pub struct SeedOptions {
    pub depth: Depth,
    pub native_buckets: usize,
    pub threads: Option<usize>,
    pub translation_cache_bytes: u64,
    pub skip_native: bool,
}

impl Default for SeedOptions {
    fn default() -> Self {
        Self {
            depth: Depth::Deep,
            native_buckets: DEFAULT_NATIVE_BUCKETS,
            threads: None,
            translation_cache_bytes: DEFAULT_TRANSLATION_CACHE_BYTES,
            skip_native: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StagedSeedFile {
    pub rel_path: String,
    pub disk_path: PathBuf,
    pub size: u64,
    pub sha256: [u8; 32],
    pub mode: u16,
}

#[derive(Clone, Debug)]
pub struct SeedOutput {
    pub stack_key: String,
    pub pack_bin: PathBuf,
    pub native_pack_dir: Option<PathBuf>,
    pub staged_files: Vec<StagedSeedFile>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
struct CrawlStamp {
    schema: u32,
    os_sha256: String,
    sys_sha256: String,
    app_sha256: String,
    qc_sha256: String,
    embedded_sha256s: Vec<String>,
    app_libraries: Vec<(String, String)>,
    files: BTreeMap<String, FileDigest>,
}

pub fn build_seed<F>(
    inputs: &SeedInputs<'_>,
    opts: &SeedOptions,
    cancel: &AtomicBool,
    on_progress: F,
) -> Result<SeedOutput>
where
    F: Fn(&str, usize, usize, &str) + Sync,
{
    if opts.native_buckets == 0 || opts.native_buckets > MAX_PLAN_BUCKETS {
        return Err(SeedError::Asset(format!(
            "native_buckets {} out of supported range 1..={MAX_PLAN_BUCKETS}",
            opts.native_buckets
        )));
    }
    check_cancel(cancel)?;
    let _cache_lock = CacheLock::acquire(inputs.work_dir)?;
    let _cache_guard = init_runtime(opts.translation_cache_bytes);

    let seed_tar_sha = inputs.seed_tar_sha256.to_ascii_lowercase();
    let unpack_id = &seed_tar_sha[..seed_tar_sha.len().min(32)];
    let unpack_dir = inputs.work_dir.join("assets").join(unpack_id);

    on_progress("unpack", 0, 1, "Validating and unpacking shader seed asset");
    let asset = unpack_seed_asset(
        inputs.seed_tar,
        inputs.build,
        inputs.qc_sha256,
        inputs.bundle_stack_key,
        &unpack_dir,
    )?;
    let stack_u64 = parse_stack_key_hex(&asset.manifest.stack_key)?;
    on_progress("unpack", 1, 1, "Shader seed asset validated");
    check_cancel(cancel)?;

    let (crawl_dir, crawl_id, qc_path) = ensure_crawl(inputs, &asset, cancel, &on_progress)?;
    check_cancel(cancel)?;

    let run_id = compute_run_id(&seed_tar_sha, &crawl_id, opts.depth);
    let run_dir = inputs.work_dir.join("runs").join(&run_id);
    fs::create_dir_all(&run_dir).map_err(|e| SeedError::io_path(&run_dir, e))?;

    let pool = build_thread_pool(opts.threads)?;
    let run_scope = alloc_scope();

    let pack_bin = build_pipeline_pack(
        &pool,
        &asset,
        &crawl_dir,
        &qc_path,
        &run_dir,
        stack_u64,
        run_scope,
        cancel,
        &on_progress,
    )?;
    check_cancel(cancel)?;

    let native_pack_dir = if opts.skip_native {
        None
    } else {
        Some(build_native_seed(
            &pool,
            &asset,
            &crawl_dir,
            &qc_path,
            &run_dir,
            opts,
            run_scope,
            cancel,
            &on_progress,
        )?)
    };

    let staged_files = collect_staged_files(&pack_bin, native_pack_dir.as_deref())?;
    Ok(SeedOutput {
        stack_key: asset.manifest.stack_key,
        pack_bin,
        native_pack_dir,
        staged_files,
    })
}

fn compute_run_id(seed_tar_sha: &str, crawl_id: &str, depth: Depth) -> String {
    let abi = desktop_compiler_abi();
    let mut h = Sha256::new();
    h.update(b"switch-seed-run-v");
    h.update(RUN_SCHEMA_VERSION.to_le_bytes());
    h.update(seed_tar_sha.as_bytes());
    h.update(b"\0");
    h.update(crawl_id.as_bytes());
    h.update(b"\0");
    h.update(depth.as_str().as_bytes());
    h.update(b"\0");
    for word in abi {
        h.update(word.to_le_bytes());
    }
    h.update(CACHE_ENTRY_FORMAT.to_le_bytes());
    let digest = hex::encode(h.finalize());
    format!("{}-{}", &digest[..32], depth.as_str())
}

fn build_thread_pool(threads: Option<usize>) -> Result<rayon::ThreadPool> {
    let n = threads.unwrap_or_else(|| {
        std::thread::available_parallelism()
            .map(std::num::NonZeroUsize::get)
            .unwrap_or(4)
    });
    // Musl defaults secondary thread stacks to 128 KiB and Windows to 1 MiB; Mesa NIR/NAK and m2v need more.
    rayon::ThreadPoolBuilder::new()
        .num_threads(n.max(1))
        .stack_size(WORKER_STACK_BYTES)
        .thread_name(|i| format!("switch-seed-{i}"))
        .build()
        .map_err(|e| SeedError::ThreadPool(e.to_string()))
}

fn resolve_member_sha(path: &Path, pinned: Option<&str>) -> Result<String> {
    if let Some(sha) = pinned {
        return Ok(sha.to_ascii_lowercase());
    }
    Ok(hex::encode(sha256_file(path)?.1))
}

fn ensure_crawl<F>(
    inputs: &SeedInputs<'_>,
    asset: &UnpackedSeedAsset,
    cancel: &AtomicBool,
    on_progress: &F,
) -> Result<(PathBuf, String, PathBuf)>
where
    F: Fn(&str, usize, usize, &str) + Sync,
{
    let os_sha256 = resolve_member_sha(inputs.os_container, inputs.os_sha256)?;
    let sys_sha256 = resolve_member_sha(inputs.system_cryptex, inputs.system_cryptex_sha256)?;
    let app_sha256 = resolve_member_sha(inputs.app_cryptex, inputs.app_cryptex_sha256)?;
    let embedded_sha256s: Vec<String> = asset
        .manifest
        .embedded
        .iter()
        .map(|e| e.sha256.to_ascii_lowercase())
        .collect();
    let mut app_libraries: Vec<(String, String)> = asset
        .manifest
        .app_libraries
        .iter()
        .map(|a| (a.guest_path.clone(), a.sha256.to_ascii_lowercase()))
        .collect();
    app_libraries.sort();

    let mut id_hasher = Sha256::new();
    id_hasher.update(CRAWL_SCHEMA_VERSION.to_le_bytes());
    for part in [
        os_sha256.as_str(),
        sys_sha256.as_str(),
        app_sha256.as_str(),
        asset.manifest.quartzcore_metallib_sha256.as_str(),
    ] {
        id_hasher.update(part.as_bytes());
        id_hasher.update(b"\0");
    }
    for emb in &embedded_sha256s {
        id_hasher.update(emb.as_bytes());
        id_hasher.update(b"\0");
    }
    id_hasher.update(b"\x01");
    for (guest_path, sha) in &app_libraries {
        id_hasher.update(guest_path.as_bytes());
        id_hasher.update(b"\0");
        id_hasher.update(sha.as_bytes());
        id_hasher.update(b"\0");
    }
    let crawl_id = hex::encode(id_hasher.finalize());
    let crawl_dir = inputs.work_dir.join("crawls").join(&crawl_id[..32]);
    let stamp_path = crawl_dir.join(".crawl.stamp.json");
    let qc_path = crawl_dir.join("root").join(QC_REL_PATH);

    if stamp_path.is_file()
        && qc_path.is_file()
        && let Ok(text) = fs::read_to_string(&stamp_path)
        && let Ok(got) = serde_json::from_str::<CrawlStamp>(&text)
        && got.schema == CRAWL_SCHEMA_VERSION
        && got.os_sha256 == os_sha256
        && got.sys_sha256 == sys_sha256
        && got.app_sha256 == app_sha256
        && got.qc_sha256 == asset.manifest.quartzcore_metallib_sha256
        && got.embedded_sha256s == embedded_sha256s
        && got.app_libraries == app_libraries
        && verify_inventory(&crawl_dir, &got.files)
    {
        on_progress("crawl", 3, 3, "Reusing cached Metal library crawl");
        return Ok((crawl_dir, crawl_id, qc_path));
    }

    if let Some(parent) = crawl_dir.parent() {
        fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
    }
    let tmp_dir = unique_temp_sibling(&crawl_dir, "crawl");
    let crawl_res = (|| -> Result<CrawlStamp> {
        let mut crawler = Crawler::new(&tmp_dir)?;
        let images = [
            ("OS container", inputs.os_container),
            ("SystemOS cryptex", inputs.system_cryptex),
            ("AppOS cryptex", inputs.app_cryptex),
        ];
        for (idx, (label, path)) in images.iter().enumerate() {
            check_cancel(cancel)?;
            on_progress(
                "crawl",
                idx,
                images.len(),
                &format!("Crawling Metal libraries in {label}"),
            );
            crawler.crawl_image_cancelable(path, cancel).map_err(|e| match e {
                CrawlError::Cancelled => SeedError::Cancelled,
                other => SeedError::Crawl(other),
            })?;
        }

        let data_dir = tmp_dir.join("data");
        for emb in &asset.embedded_files {
            let name = emb
                .file_name()
                .ok_or_else(|| SeedError::Asset("embedded file missing name".into()))?;
            fs::copy(emb, data_dir.join(name)).map_err(|e| SeedError::io_path(emb, e))?;
        }

        let root_dir = tmp_dir.join("root");
        for (guest_path, src) in &asset.app_libraries {
            let dest = root_dir.join(guest_path);
            if let Some(parent) = dest.parent() {
                fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
            }
            fs::copy(src, &dest).map_err(|e| SeedError::io_path(&dest, e))?;
        }

        let tmp_qc = tmp_dir.join("root").join(QC_REL_PATH);
        if !tmp_qc.is_file() {
            return Err(SeedError::Asset(format!(
                "crawled images missing QuartzCore library {QC_REL_PATH}"
            )));
        }
        let got_qc_sha = hex::encode(sha256_file(&tmp_qc)?.1);
        if !got_qc_sha.eq_ignore_ascii_case(&asset.manifest.quartzcore_metallib_sha256) {
            return Err(SeedError::QuartzCoreMismatch {
                got: got_qc_sha,
                expected: asset.manifest.quartzcore_metallib_sha256.clone(),
            });
        }

        let inventory = collect_inventory(&tmp_dir)?;
        Ok(CrawlStamp {
            schema: CRAWL_SCHEMA_VERSION,
            os_sha256,
            sys_sha256,
            app_sha256,
            qc_sha256: asset.manifest.quartzcore_metallib_sha256.clone(),
            embedded_sha256s,
            app_libraries,
            files: inventory,
        })
    })();

    let stamp = match crawl_res {
        Ok(s) => s,
        Err(err) => {
            let _ = fs::remove_dir_all(&tmp_dir);
            return Err(err);
        }
    };
    fs::write(tmp_dir.join(".crawl.stamp.json"), serde_json::to_vec_pretty(&stamp)?)?;
    if let Err(err) = replace_dir_atomic(&tmp_dir, &crawl_dir) {
        let _ = fs::remove_dir_all(&tmp_dir);
        return Err(err);
    }
    on_progress("crawl", 3, 3, "Metal library crawl complete");
    Ok((crawl_dir, crawl_id, qc_path))
}

fn collect_staged_files(pack_bin: &Path, native_dir: Option<&Path>) -> Result<Vec<StagedSeedFile>> {
    let mut files = Vec::new();
    let (pack_size, pack_sha) = sha256_file(pack_bin)?;
    files.push(StagedSeedFile {
        rel_path: format!("{PACK_REL_DIR}/pack.bin"),
        disk_path: pack_bin.to_path_buf(),
        size: pack_size,
        sha256: pack_sha,
        mode: 0o644,
    });

    if let Some(native) = native_dir {
        let mut raw_files = Vec::new();
        collect_sorted_files(native, &mut raw_files)?;
        for path in raw_files {
            let rel = path.strip_prefix(native).map_err(|e| SeedError::Asset(e.to_string()))?;
            let mut comps = rel.components();
            let Some(first) = comps.next() else {
                continue;
            };
            let top = first.as_os_str().to_string_lossy();
            if !top.starts_with("rec-") {
                continue;
            }
            let inner = rel
                .strip_prefix(first)
                .map_err(|e| SeedError::Asset(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            if inner.is_empty() || !staged_in_pack(&native.join(top.as_ref()), &inner)? {
                continue;
            }
            let rel_posix = rel.to_string_lossy().replace('\\', "/");
            let (size, sha256) = sha256_file(&path)?;
            files.push(StagedSeedFile {
                rel_path: format!("{PACK_REL_DIR}/native/{rel_posix}"),
                disk_path: path,
                size,
                sha256,
                mode: 0o644,
            });
        }
    }
    files.sort_by(|a, b| a.rel_path.cmp(&b.rel_path));
    Ok(files)
}
