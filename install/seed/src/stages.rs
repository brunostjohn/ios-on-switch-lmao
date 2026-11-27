// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use rayon::prelude::*;
use serde_json::Value;
use switch_seed_crawl::{index_names, resolve_corpus};
use switch_seed_link::records::read_records;
use switch_seed_link::{LinkConfig, link_buckets};
use switch_seed_plan::{PlanConfig, pack_native_seed, restamp_buckets, run_plan, split_native_lines};

use crate::asset::UnpackedSeedAsset;
use crate::error::{Result, SeedError};
use crate::ffi::{DirectpackArgs, NativeBucketArgs, directpack, native_bucket};
use crate::fsutil::{
    FileDigest, OutputStamp, check_cancel, collect_inventory, replace_dir_atomic, replace_file_atomic, sha256_file,
    unique_temp_sibling, verify_inventory,
};
use crate::pipeline::SeedOptions;

#[allow(clippy::too_many_arguments)]
pub(crate) fn build_pipeline_pack<F>(
    pool: &rayon::ThreadPool,
    asset: &UnpackedSeedAsset,
    crawl_dir: &Path,
    qc_path: &Path,
    run_dir: &Path,
    stack_u64: u64,
    run_scope: u64,
    cancel: &AtomicBool,
    on_progress: &F,
) -> Result<PathBuf>
where
    F: Fn(&str, usize, usize, &str) + Sync,
{
    let out_dir = run_dir.join("out/pack");
    let pack_bin = out_dir.join("pack.bin");
    let pack_stamp = out_dir.join(".pack.stamp.json");
    if pack_stamp.is_file()
        && pack_bin.is_file()
        && let Ok(text) = fs::read_to_string(&pack_stamp)
        && let Ok(stamp) = serde_json::from_str::<OutputStamp>(&text)
        && stamp.files.contains_key("pack.bin")
        && verify_inventory(&out_dir, &stamp.files)
    {
        on_progress(
            "pack",
            asset.split_buckets.len(),
            asset.split_buckets.len(),
            "Reusing cached pipeline pack",
        );
        return Ok(pack_bin);
    }

    let buckets_root = run_dir.join("pack_buckets");
    fs::create_dir_all(&buckets_root).map_err(|e| SeedError::io_path(&buckets_root, e))?;
    let lib_root = crawl_dir.join("root");
    let data_libs = crawl_dir.join("data");
    let total = asset.split_buckets.len();
    let completed = AtomicUsize::new(0);

    // Run bucket 0 on the 16 MiB Rayon pool (never the caller's stack) before fanning out.
    pool.install(|| {
        run_pack_bucket(
            0,
            &asset.split_buckets[0],
            asset,
            qc_path,
            &lib_root,
            &data_libs,
            &buckets_root,
            run_scope,
            cancel,
        )
    })?;
    let done0 = completed.fetch_add(1, Ordering::Relaxed) + 1;
    on_progress(
        "pack",
        done0,
        total,
        &format!("Compiled pipeline bucket {done0}/{total}"),
    );

    let rest: Vec<(usize, &PathBuf)> = asset.split_buckets.iter().enumerate().skip(1).collect();
    let results: Vec<Result<()>> = pool.install(|| {
        rest.par_iter()
            .map(|&(idx, corpus)| {
                run_pack_bucket(
                    idx,
                    corpus,
                    asset,
                    qc_path,
                    &lib_root,
                    &data_libs,
                    &buckets_root,
                    run_scope,
                    cancel,
                )?;
                let done = completed.fetch_add(1, Ordering::Relaxed) + 1;
                on_progress("pack", done, total, &format!("Compiled pipeline bucket {done}/{total}"));
                Ok(())
            })
            .collect()
    });
    for r in results {
        r?;
    }
    check_cancel(cancel)?;

    on_progress("link", 0, 1, "Linking and stamping GPU pipeline pack");
    if let Some(parent) = out_dir.parent() {
        fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
    }
    let tmp_out = unique_temp_sibling(&out_dir, "pack");
    fs::create_dir_all(&tmp_out).map_err(|e| SeedError::io_path(&tmp_out, e))?;

    let bucket_dirs: Vec<PathBuf> = (0..total).map(|i| buckets_root.join(format!("bucket{i}"))).collect();
    let cfg = LinkConfig {
        buckets: bucket_dirs,
        out_dir: tmp_out.clone(),
        context_log: Some(asset.context_log.clone()),
        draw_context: Some(asset.draw_context.clone()),
        archives: asset.archives_tsv.clone(),
        corpus_report: Some(asset.corpus_report.clone()),
        stack: Some(stack_u64),
        write_debug_outputs: false,
    };
    if let Err(err) = link_buckets(&cfg) {
        let _ = fs::remove_dir_all(&tmp_out);
        return Err(err.into());
    }
    check_cancel(cancel)?;
    let (pack_size, pack_sha) = sha256_file(&tmp_out.join("pack.bin"))?;
    let mut files = BTreeMap::new();
    files.insert(
        "pack.bin".to_owned(),
        FileDigest {
            size: pack_size,
            sha256: hex::encode(pack_sha),
        },
    );
    fs::write(
        tmp_out.join(".pack.stamp.json"),
        serde_json::to_vec_pretty(&OutputStamp { files })?,
    )?;
    if let Err(err) = replace_dir_atomic(&tmp_out, &out_dir) {
        let _ = fs::remove_dir_all(&tmp_out);
        return Err(err);
    }
    on_progress("link", 1, 1, "GPU pipeline pack linked");
    Ok(pack_bin)
}

#[allow(clippy::too_many_arguments)]
fn run_pack_bucket(
    idx: usize,
    corpus: &Path,
    asset: &UnpackedSeedAsset,
    qc_path: &Path,
    lib_root: &Path,
    data_libs: &Path,
    buckets_root: &Path,
    run_scope: u64,
    cancel: &AtomicBool,
) -> Result<()> {
    check_cancel(cancel)?;
    let bdir = buckets_root.join(format!("bucket{idx}"));
    let rec_path = bdir.join("records.bin");
    let stamp_path = bdir.join(".done.json");
    if stamp_path.is_file()
        && rec_path.is_file()
        && let Ok(text) = fs::read_to_string(&stamp_path)
        && let Ok(stamp) = serde_json::from_str::<OutputStamp>(&text)
        && stamp.files.contains_key("records.bin")
        && verify_inventory(&bdir, &stamp.files)
    {
        return Ok(());
    }
    fs::create_dir_all(&bdir).map_err(|e| SeedError::io_path(&bdir, e))?;
    let tmp_rec = unique_temp_sibling(&rec_path, "records");
    let res = directpack(&DirectpackArgs {
        kind: &asset.manifest.kind,
        qc_metallib: qc_path,
        corpus_path: corpus,
        output_path: &tmp_rec,
        own_lib: (idx == 0).then_some(asset.own_metallib.as_path()),
        lib_root: Some(lib_root),
        data_libs: Some(data_libs),
        label_prefix: None,
        scope: run_scope,
        cancel,
    });
    if let Err(err) = res {
        let _ = fs::remove_file(&tmp_rec);
        return Err(err);
    }

    let parsed = match read_records(&tmp_rec) {
        Ok(p) => p,
        Err(err) => {
            let _ = fs::remove_file(&tmp_rec);
            return Err(err.into());
        }
    };
    let made_total = parsed.states.iter().filter(|s| s.outcome == 0).count()
        + parsed.kernels.iter().filter(|k| k.outcome == 0).count();
    let failed_state = parsed.states.iter().find(|s| s.outcome == 2);
    let failed_kernel = parsed.kernels.iter().find(|k| k.outcome == 2);
    if made_total == 0
        && let Some((kind, key, why)) = failed_state
            .map(|s| ("render", s.key.as_str(), s.why.as_str()))
            .or_else(|| failed_kernel.map(|k| ("compute", k.key.as_str(), k.why.as_str())))
    {
        let _ = fs::remove_file(&tmp_rec);
        return Err(SeedError::Ffi {
            op: "directpack",
            detail: format!("bucket{idx} produced no states and failed {kind} {key}: {why}"),
        });
    }
    drop(parsed);

    let (size, sha) = sha256_file(&tmp_rec)?;
    replace_file_atomic(&tmp_rec, &rec_path)?;
    let mut files = BTreeMap::new();
    files.insert(
        "records.bin".to_owned(),
        FileDigest {
            size,
            sha256: hex::encode(sha),
        },
    );
    let tmp_stamp = unique_temp_sibling(&stamp_path, "done");
    fs::write(&tmp_stamp, serde_json::to_vec_pretty(&OutputStamp { files })?)?;
    replace_file_atomic(&tmp_stamp, &stamp_path)?;
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn build_native_seed<F>(
    pool: &rayon::ThreadPool,
    asset: &UnpackedSeedAsset,
    crawl_dir: &Path,
    qc_path: &Path,
    run_dir: &Path,
    opts: &SeedOptions,
    run_scope: u64,
    cancel: &AtomicBool,
    on_progress: &F,
) -> Result<PathBuf>
where
    F: Fn(&str, usize, usize, &str) + Sync,
{
    let native_tag = format!("native-{}", opts.native_buckets);
    let native_out = run_dir.join("out").join(&native_tag);
    let rec_dir_name = format!("rec-{}", asset.manifest.stack_key);
    let native_stamp = native_out.join(".native.stamp.json");
    if native_stamp.is_file()
        && let Ok(text) = fs::read_to_string(&native_stamp)
        && let Ok(stamp) = serde_json::from_str::<OutputStamp>(&text)
        && stamp.files.contains_key(&format!("{rec_dir_name}/index"))
        && verify_inventory(&native_out, &stamp.files)
    {
        on_progress(
            "native",
            opts.native_buckets,
            opts.native_buckets,
            "Reusing cached native shader seed",
        );
        return Ok(native_out);
    }

    let plan_dir = run_dir.join(format!("native_plan_{}", opts.native_buckets));
    let buckets_dir = plan_dir.join("buckets");
    let store_json = plan_dir.join("store.json");
    let plan_done = plan_dir.join(".plan.done");
    if !plan_done.is_file() || !store_json.is_file() {
        check_cancel(cancel)?;
        on_progress("native_plan", 0, 1, "Resolving and planning native shader buckets");
        let tmp_plan = unique_temp_sibling(&plan_dir, "native_plan");
        fs::create_dir_all(&tmp_plan).map_err(|e| SeedError::io_path(&tmp_plan, e))?;
        let plan_res = (|| -> Result<()> {
            let names = index_names(&asset.manifest.build, &crawl_dir.join("root"), None)?;
            let corpus_text =
                fs::read_to_string(&asset.corpus_log).map_err(|e| SeedError::io_path(&asset.corpus_log, e))?;
            let resolved = resolve_corpus(&names, &corpus_text);
            let resolved_path = tmp_plan.join("corpus.log");
            fs::write(&resolved_path, &resolved.text).map_err(|e| SeedError::io_path(&resolved_path, e))?;
            check_cancel(cancel)?;
            run_plan(&PlanConfig {
                build: Some(asset.manifest.kind.clone()),
                corpus: resolved_path,
                root: crawl_dir.join("root"),
                data_files: Vec::new(),
                data_dirs: vec![crawl_dir.join("data")],
                qc: qc_path.to_path_buf(),
                qc_sha256: asset.manifest.quartzcore_metallib_sha256.clone(),
                depth: opts.depth,
                buckets_dir: tmp_plan.join("buckets"),
                count: opts.native_buckets,
                store: tmp_plan.join("store.json"),
                report: tmp_plan.join("plan.json"),
                input_reports: Vec::new(),
                apps: 0,
                base_root: None,
                base_data_dirs: Vec::new(),
                base_store: None,
            })?;
            fs::write(tmp_plan.join(".plan.done"), b"ok\n")?;
            Ok(())
        })();
        if let Err(err) = plan_res {
            let _ = fs::remove_dir_all(&tmp_plan);
            return Err(err);
        }
        replace_dir_atomic(&tmp_plan, &plan_dir)?;
        on_progress("native_plan", 1, 1, "Native shader buckets planned");
    }

    populate_store_data_links(&store_json, &crawl_dir.join("data"))?;

    let nb_root = run_dir.join(format!("native_buckets_{}", opts.native_buckets));
    fs::create_dir_all(&nb_root).map_err(|e| SeedError::io_path(&nb_root, e))?;
    let total = opts.native_buckets;
    let completed = AtomicUsize::new(0);
    let indices: Vec<usize> = (0..total).collect();
    let results: Vec<Result<()>> = pool.install(|| {
        indices
            .par_iter()
            .map(|&idx| {
                run_native_bucket(
                    idx,
                    asset,
                    crawl_dir,
                    qc_path,
                    &buckets_dir,
                    &nb_root,
                    run_scope,
                    cancel,
                )?;
                let done = completed.fetch_add(1, Ordering::Relaxed) + 1;
                on_progress(
                    "native",
                    done,
                    total,
                    &format!("Compiled native shader bucket {done}/{total}"),
                );
                Ok(())
            })
            .collect()
    });
    for r in results {
        r?;
    }
    check_cancel(cancel)?;

    on_progress("native_pack", 0, 1, "Stamping and packing native shader seed");
    let bucket_dirs: Vec<PathBuf> = (0..total).map(|i| nb_root.join(format!("bucket-{i:03}"))).collect();
    let stamped_dir = unique_temp_sibling(&run_dir.join("native_stamped"), "stamped");
    if let Some(parent) = native_out.parent() {
        fs::create_dir_all(parent).map_err(|e| SeedError::io_path(parent, e))?;
    }
    let tmp_native = unique_temp_sibling(&native_out, "native_out");
    let pack_res = (|| -> Result<OutputStamp> {
        restamp_buckets(&asset.manifest.stack_key, &stamped_dir, &bucket_dirs)?;
        check_cancel(cancel)?;
        pack_native_seed(&stamped_dir, &tmp_native)?;
        check_cancel(cancel)?;
        Ok(OutputStamp {
            files: collect_inventory(&tmp_native)?,
        })
    })();
    let _ = fs::remove_dir_all(&stamped_dir);
    let stamp = match pack_res {
        Ok(s) => s,
        Err(err) => {
            let _ = fs::remove_dir_all(&tmp_native);
            return Err(err);
        }
    };
    fs::write(
        tmp_native.join(".native.stamp.json"),
        serde_json::to_vec_pretty(&stamp)?,
    )?;
    if let Err(err) = replace_dir_atomic(&tmp_native, &native_out) {
        let _ = fs::remove_dir_all(&tmp_native);
        return Err(err);
    }
    on_progress("native_pack", 1, 1, "Native shader seed packed");
    Ok(native_out)
}

fn populate_store_data_links(store_json: &Path, data_dir: &Path) -> Result<()> {
    let text = fs::read_to_string(store_json).map_err(|e| SeedError::io_path(store_json, e))?;
    let by_sha: BTreeMap<String, String> = serde_json::from_str(&text)?;
    fs::create_dir_all(data_dir).map_err(|e| SeedError::io_path(data_dir, e))?;
    for (sha, src_str) in by_sha {
        if sha.len() != 64 || !sha.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')) {
            return Err(SeedError::Asset(format!("invalid store.json sha256 {sha:?}")));
        }
        let dest = data_dir.join(format!("{sha}.metallib"));
        if dest.is_file() && sha256_file(&dest).is_ok_and(|(_, d)| hex::encode(d) == sha) {
            continue;
        }
        let src = Path::new(&src_str);
        if dest.exists() {
            let _ = fs::remove_file(&dest);
        }
        if fs::hard_link(src, &dest).is_err() {
            let tmp = unique_temp_sibling(&dest, "metallib");
            if let Err(err) = fs::copy(src, &tmp) {
                let _ = fs::remove_file(&tmp);
                return Err(SeedError::io_path(&dest, err));
            }
            if let Err(err) = replace_file_atomic(&tmp, &dest) {
                let _ = fs::remove_file(&tmp);
                return Err(err);
            }
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn run_native_bucket(
    idx: usize,
    asset: &UnpackedSeedAsset,
    crawl_dir: &Path,
    qc_path: &Path,
    plan_buckets_dir: &Path,
    nb_root: &Path,
    run_scope: u64,
    cancel: &AtomicBool,
) -> Result<()> {
    check_cancel(cancel)?;
    let name = format!("bucket-{idx:03}");
    let out_dir = nb_root.join(&name);
    let stamp_path = out_dir.join(".done.json");
    if stamp_path.is_file()
        && out_dir.join("manifest.json").is_file()
        && let Ok(text) = fs::read_to_string(&stamp_path)
        && let Ok(stamp) = serde_json::from_str::<OutputStamp>(&text)
        && stamp.files.contains_key("manifest.json")
        && verify_inventory(&out_dir, &stamp.files)
    {
        return Ok(());
    }
    let tmp_dir = unique_temp_sibling(&out_dir, &name);
    fs::create_dir_all(&tmp_dir).map_err(|e| SeedError::io_path(&tmp_dir, e))?;

    let build_res = (|| -> Result<OutputStamp> {
        let log_name = format!("{name}.log");
        let bucket_log = plan_buckets_dir.join(&log_name);
        let text = fs::read_to_string(&bucket_log).map_err(|e| SeedError::io_path(&bucket_log, e))?;
        let (observed, derived) = split_native_lines(&text);
        let obs_path = tmp_dir.join("observed.log");
        let der_path = tmp_dir.join("derived.log");
        write_lines(&obs_path, &observed)?;
        write_lines(&der_path, &derived)?;

        let corpora = [obs_path.as_path()];
        let derived_paths = [der_path.as_path()];
        native_bucket(&NativeBucketArgs {
            kind: &asset.manifest.kind,
            libs_store: crawl_dir,
            qc_metallib: qc_path,
            stack_key: 0,
            output_dir: &tmp_dir,
            corpora: &corpora,
            derived: &derived_paths,
            scope: run_scope,
            cancel,
        })?;

        let manifest_path = tmp_dir.join("manifest.json");
        let mut manifest: Value = serde_json::from_str(&fs::read_to_string(&manifest_path)?)?;
        if let Some(records) = manifest.get_mut("records").and_then(Value::as_array_mut) {
            for row in records {
                if let Some(obj) = row.as_object_mut() {
                    obj.insert("source".to_owned(), Value::String(log_name.clone()));
                }
            }
        }
        fs::write(&manifest_path, serde_json::to_string(&manifest)? + "\n")
            .map_err(|e| SeedError::io_path(&manifest_path, e))?;
        let _ = fs::remove_file(&obs_path);
        let _ = fs::remove_file(&der_path);
        Ok(OutputStamp {
            files: collect_inventory(&tmp_dir)?,
        })
    })();

    let stamp = match build_res {
        Ok(s) => s,
        Err(err) => {
            let _ = fs::remove_dir_all(&tmp_dir);
            return Err(err);
        }
    };
    fs::write(tmp_dir.join(".done.json"), serde_json::to_vec_pretty(&stamp)?)?;
    if let Err(err) = replace_dir_atomic(&tmp_dir, &out_dir) {
        let _ = fs::remove_dir_all(&tmp_dir);
        return Err(err);
    }
    Ok(())
}

fn write_lines(path: &Path, lines: &[String]) -> Result<()> {
    let mut text = String::new();
    for line in lines {
        text.push_str(line);
        text.push('\n');
    }
    fs::write(path, text).map_err(|e| SeedError::io_path(path, e))
}
