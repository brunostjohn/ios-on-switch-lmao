// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

use sha2::{Digest, Sha256};
use switch_apfs_read::Fs;
use switch_apfs_write::{Attrs, Content, InodeRef, Kind, Tree};
use switch_catalog::{Build, Role};
use switch_image::FileFill;
use switch_image_gpu::{FileContent, FileRecord, GpuStage, stage_gpu};
use switch_image_overlay::{SourceKind, TreeSource, merge_tree};
use switch_seed::{
    SeedError, SeedInputs, SeedOptions, SeedOutput, build_seed, is_seed_asset_tar, parse_stack_key_hex,
    read_bundle_stack_key,
};

use crate::LocalInputs;
use crate::error::{EngineError, Result};
use crate::fetch::{PreparedAssets, PreparedMembers, check_asset_matches};
use crate::progress::EngineProgress;

const BUNDLE_PLIST_PATH: &str = "ngpu/addons/TegraGM20BMetal.bundle/Info.plist";

pub enum LoadedGpu {
    Stage(Box<GpuStage>),
    PlainDir(PathBuf),
    None,
}

pub enum PreparedSeed {
    Built(SeedOutput),
    PrebuiltTree(TreeSource),
}

fn is_tar_path(path: &Path) -> bool {
    path.is_file()
        && path
            .extension()
            .and_then(|ext| ext.to_str())
            .is_some_and(|ext| ext.eq_ignore_ascii_case("tar"))
}

pub fn load_gpu(
    build: &Build,
    local: &LocalInputs,
    assets: &PreparedAssets,
    progress: &mut EngineProgress<'_>,
) -> Result<LoadedGpu> {
    if let Some(gpu_path) = &local.gpu_dir {
        if is_tar_path(gpu_path) {
            progress.log(format!("Loading GPU bundle tar from {}", gpu_path.display()));
            let file = File::open(gpu_path).map_err(|e| EngineError::io_path(gpu_path, e))?;
            let stage = GpuStage::from_tar(BufReader::new(file))?;
            return Ok(LoadedGpu::Stage(Box::new(stage)));
        }
        if gpu_path.is_file() || gpu_path.join(switch_image_gpu::TREE_MANIFEST).is_file() {
            progress.log(format!("Loading GPU bundle manifest from {}", gpu_path.display()));
            let stage = GpuStage::from_path(gpu_path)?;
            return Ok(LoadedGpu::Stage(Box::new(stage)));
        }
        return Ok(LoadedGpu::PlainDir(gpu_path.clone()));
    }
    if let Some(tar_path) = &assets.gpu_tar {
        progress.log(format!("Loading GPU release asset from {}", tar_path.display()));
        let asset = build.asset("gpu")?;
        let file = File::open(tar_path).map_err(|e| EngineError::io_path(tar_path, e))?;
        let mut checked = asset.checked(BufReader::new(file));
        let stage = GpuStage::from_tar(&mut checked)?;
        checked.finish()?;
        return Ok(LoadedGpu::Stage(Box::new(stage)));
    }
    Ok(LoadedGpu::None)
}

pub fn bundle_stack_key(gpu: &LoadedGpu) -> Result<Option<String>> {
    let bytes = match gpu {
        LoadedGpu::None => return Ok(None),
        LoadedGpu::Stage(stage) => {
            let Some(rec) = stage.files.get(BUNDLE_PLIST_PATH) else {
                return Ok(None);
            };
            match &rec.content {
                FileContent::Bytes(b) => b.to_vec(),
                FileContent::Path(p) => fs::read(p).map_err(|e| EngineError::io_path(p, e))?,
            }
        }
        LoadedGpu::PlainDir(dir) => {
            let plist_path = dir.join(BUNDLE_PLIST_PATH);
            if !plist_path.is_file() {
                return Ok(None);
            }
            fs::read(&plist_path).map_err(|e| EngineError::io_path(&plist_path, e))?
        }
    };
    let Some(raw) = read_bundle_stack_key(&bytes)? else {
        return Ok(None);
    };
    let stack = parse_stack_key_hex(&raw)?;
    Ok(Some(format!("{stack:016x}")))
}

fn required_bundle_stack_key(gpu: &LoadedGpu) -> Result<Option<String>> {
    match gpu {
        LoadedGpu::None => Ok(None),
        LoadedGpu::Stage(_) | LoadedGpu::PlainDir(_) => {
            let Some(key) = bundle_stack_key(gpu)? else {
                return Err(EngineError::Config(format!(
                    "GPU bundle missing SwitchShaderStack in {BUNDLE_PLIST_PATH}"
                )));
            };
            Ok(Some(key))
        }
    }
}

pub fn prepare_seed(
    build: &Build,
    local: &LocalInputs,
    assets: &PreparedAssets,
    members: &PreparedMembers,
    gpu: &LoadedGpu,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<Option<PreparedSeed>> {
    if let Some(seed_path) = &local.shader_seed_dir {
        if is_seed_asset_tar(seed_path)? {
            let bundle_stack = required_bundle_stack_key(gpu)?;
            let tar_sha = sha256_path(seed_path)?;
            let out = run_seed_pipeline(
                build,
                local,
                seed_path,
                &tar_sha,
                members,
                bundle_stack.as_deref(),
                work_dir,
                progress,
            )?;
            return Ok(Some(PreparedSeed::Built(out)));
        }
        progress.log(format!("Loading prebuilt shader seed from {}", seed_path.display()));
        let src = if seed_path.is_dir() {
            TreeSource::from_dir(seed_path)?
        } else {
            let file = File::open(seed_path).map_err(|e| EngineError::io_path(seed_path, e))?;
            TreeSource::from_tar(BufReader::new(file))?
        };
        return Ok(Some(PreparedSeed::PrebuiltTree(src)));
    }

    if let Some(tar_path) = &assets.seed_tar {
        let asset = build.asset("seed")?;
        check_asset_matches(asset, tar_path)?;
        let bundle_stack = required_bundle_stack_key(gpu)?;
        let tar_sha = asset.sha256.to_string();
        let out = run_seed_pipeline(
            build,
            local,
            tar_path,
            &tar_sha,
            members,
            bundle_stack.as_deref(),
            work_dir,
            progress,
        )?;
        return Ok(Some(PreparedSeed::Built(out)));
    }

    Ok(None)
}

#[allow(clippy::too_many_arguments)]
fn run_seed_pipeline(
    build: &Build,
    local: &LocalInputs,
    seed_tar: &Path,
    seed_tar_sha256: &str,
    members: &PreparedMembers,
    bundle_stack: Option<&str>,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<SeedOutput> {
    let qc_sha256 = build.record.derived.quartzcore_metallib_sha256.to_string();
    let seed_work = work_dir.join("cache").join(build.build()).join("seed");
    let cancel_arc = if progress.is_irreversible() {
        Arc::new(AtomicBool::new(false))
    } else {
        progress.cancel_token().into_arc()
    };
    let os_sha = local
        .os_dmg
        .is_none()
        .then(|| build.container(Role::Os).map(|(_, p)| p.sha256.to_string()))
        .flatten();
    let sys_sha = local
        .system_cryptex
        .is_none()
        .then(|| build.container(Role::SystemCryptex).map(|(_, p)| p.sha256.to_string()))
        .flatten();
    let app_sha = local
        .app_cryptex
        .is_none()
        .then(|| build.container(Role::AppCryptex).map(|(_, p)| p.sha256.to_string()))
        .flatten();

    progress.log(format!(
        "Compiling GPU shader seed for {} from {}",
        build.build(),
        seed_tar.display()
    ));

    let inputs = SeedInputs {
        build: build.build(),
        qc_sha256: &qc_sha256,
        seed_tar,
        seed_tar_sha256,
        os_container: &members.os_container,
        system_cryptex: &members.system_cryptex,
        app_cryptex: &members.app_cryptex,
        os_sha256: os_sha.as_deref(),
        system_cryptex_sha256: sys_sha.as_deref(),
        app_cryptex_sha256: app_sha.as_deref(),
        bundle_stack_key: bundle_stack,
        work_dir: &seed_work,
    };
    let opts = SeedOptions::default();

    let progress_mtx = Mutex::new(progress);
    let out = build_seed(&inputs, &opts, &cancel_arc, |stage, done, total, detail| {
        if let Ok(mut p) = progress_mtx.lock() {
            let force = done == 0 || done == total;
            p.step_progress(
                "seed",
                done as u64,
                Some(total as u64),
                Some(format!("{stage}: {detail}")),
                force,
            );
        }
    })
    .map_err(|e| match e {
        SeedError::Cancelled => EngineError::Cancelled,
        other => EngineError::Seed(other),
    })?;
    if let Ok(mut p) = progress_mtx.lock() {
        p.log(format!(
            "Shader seed ready (stack {}, {} staged files)",
            out.stack_key,
            out.staged_files.len()
        ));
    }
    Ok(out)
}

pub fn stage_gpu_and_seed(
    data_tree: &mut Tree,
    system_fs: &Fs<'_>,
    gpu: LoadedGpu,
    seed: Option<PreparedSeed>,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    match gpu {
        LoadedGpu::Stage(mut stage) => {
            let mut extra_tree: Option<TreeSource> = None;
            match seed {
                Some(PreparedSeed::Built(out)) => {
                    for f in out.staged_files {
                        if stage.files.contains_key(&f.rel_path) {
                            return Err(EngineError::Config(format!(
                                "shader seed path {} collides with GPU bundle",
                                f.rel_path
                            )));
                        }
                        stage.files.insert(
                            f.rel_path.clone(),
                            FileRecord {
                                path: f.rel_path.clone(),
                                sha256: f.sha256,
                                size: f.size,
                                mode: f.mode,
                                source: f.rel_path,
                                content: FileContent::Path(f.disk_path),
                            },
                        );
                    }
                }
                Some(PreparedSeed::PrebuiltTree(src)) => {
                    let mut leftovers = TreeSource::new();
                    for (path, entry) in src.entries() {
                        match &entry.kind {
                            SourceKind::File(bytes) => {
                                if stage.files.contains_key(path) {
                                    return Err(EngineError::Config(format!(
                                        "prebuilt shader seed path {path} collides with GPU bundle"
                                    )));
                                }
                                let sha256: [u8; 32] = Sha256::digest(bytes).into();
                                stage.files.insert(
                                    path.clone(),
                                    FileRecord {
                                        path: path.clone(),
                                        sha256,
                                        size: bytes.len() as u64,
                                        mode: entry.mode,
                                        source: path.clone(),
                                        content: FileContent::Bytes(Arc::clone(bytes)),
                                    },
                                );
                            }
                            SourceKind::Dir => {}
                            SourceKind::Symlink(target) => {
                                leftovers.add_symlink(path, target, entry.mode)?;
                            }
                        }
                    }
                    if !leftovers.is_empty() {
                        extra_tree = Some(leftovers);
                    }
                }
                None => {}
            }
            let report = stage_gpu(data_tree, Some(system_fs), &stage)?;
            progress.log(format!(
                "GPU bundle + seed: {} files, {} pref plists, icon build {}",
                report.files, report.prefs, report.icon_build
            ));
            if let Some(src) = extra_tree {
                let rep = merge_tree(data_tree, &src)?;
                progress.log(format!(
                    "Prebuilt shader seed extras: {} files, {} dirs",
                    rep.files, rep.dirs
                ));
            }
        }
        LoadedGpu::PlainDir(dir) => {
            progress.log(format!("Merging GPU bundle from {}", dir.display()));
            let gpu_src = TreeSource::from_dir(&dir)?;
            let report = merge_tree(data_tree, &gpu_src)?;
            progress.log(format!("GPU bundle: {} files, {} dirs", report.files, report.dirs));
            stage_standalone_seed(data_tree, seed, progress)?;
        }
        LoadedGpu::None => {
            stage_standalone_seed(data_tree, seed, progress)?;
        }
    }
    Ok(())
}

fn stage_standalone_seed(
    data_tree: &mut Tree,
    seed: Option<PreparedSeed>,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    match seed {
        Some(PreparedSeed::Built(out)) => {
            let count = out.staged_files.len();
            for f in out.staged_files {
                let (parent, name) = ensure_parent_dirs(data_tree, &f.rel_path)?;
                let fill = Arc::new(FileFill::new(f.disk_path, f.size, Some(f.sha256)));
                let stream = data_tree.add_stream(f.size, Content::Fill(fill));
                if let Some(existing) = data_tree.child(parent, name) {
                    if data_tree.inode(existing).kind != Kind::File {
                        return Err(EngineError::Config(format!("{} is not a regular file", f.rel_path)));
                    }
                    data_tree.replace_content(existing, Some(stream))?;
                    let attrs = &mut data_tree.inode_mut(existing).attrs;
                    attrs.uid = 0;
                    attrs.gid = 0;
                    attrs.mode = f.mode;
                    attrs.times = None;
                } else {
                    data_tree.add_file(parent, name, Attrs::new(0, 0, f.mode), Some(stream))?;
                }
            }
            progress.log(format!("Shader seed staged: {count} files"));
        }
        Some(PreparedSeed::PrebuiltTree(src)) => {
            let report = merge_tree(data_tree, &src)?;
            progress.log(format!("Shader seed: {} files, {} dirs", report.files, report.dirs));
        }
        None => {}
    }
    Ok(())
}

fn ensure_parent_dirs<'a>(tree: &mut Tree, path: &'a str) -> Result<(InodeRef, &'a str)> {
    let (parent_path, name) = match path.rsplit_once('/') {
        Some((p, n)) => (Some(p), n),
        None => (None, path),
    };
    let mut parent = tree.root();
    if let Some(rel) = parent_path {
        for part in rel.split('/') {
            parent = match tree.child(parent, part) {
                Some(existing) => {
                    if tree.inode(existing).kind != Kind::Dir {
                        return Err(EngineError::Config(format!("{part} in {path} is not a directory")));
                    }
                    existing
                }
                None => tree.add_dir(parent, part, Attrs::new(0, 0, 0o755))?,
            };
        }
    }
    Ok((parent, name))
}

fn sha256_path(path: &Path) -> Result<String> {
    let file = File::open(path).map_err(|e| EngineError::io_path(path, e))?;
    let mut reader = BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = reader.read(&mut buf).map_err(|e| EngineError::io_path(path, e))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hex::encode(hasher.finalize()))
}
