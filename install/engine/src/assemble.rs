// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::path::Path;
use std::sync::Arc;

use switch_apfs_format::io::FileSource;
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::Fs;
use switch_apfs_seal::{SealOptions, seal_fill};
use switch_apfs_write::{Tree, VolumeSpec};
use switch_catalog::{Build, Role};
use switch_image::{
    CryptexContent, CryptexImage, DEFAULT_DATA_NAME, DEFAULT_PREBOOT_NAME, DataVolumeParams, DeltaDir,
    PrebootVolumeParams, apply_var_delta, build_preboot, data_volume_spec, load_manifest, parse_uuid,
    preboot_volume_spec, seed_data, staging_name, tree_entries,
};
use switch_image_bootstrap::{
    ARCHIVE, BootstrapPins, HELPER, MANIFEST, stage_bootstrap_from_dir, stage_bootstrap_reader,
};
use switch_image_overlay::{SourceKind, TreeSource, merge_tree, place_overlay, stage_apps};
use switch_image_pretranslated::{parse_cdhash_pairs, place_pretranslated};

use crate::boot::BootOutputs;
use crate::error::{EngineError, Result};
use crate::fetch::PreparedMembers;
use crate::pretranslate::PreparedDeltas;
use crate::progress::EngineProgress;
use crate::{DATA_VOLUME_UUID_STR, LocalInputs, PREBOOT_VOLUME_UUID_STR};

pub struct AssembledVolumes {
    pub data_spec: VolumeSpec,
    pub data_tree: Tree,
    pub preboot_spec: VolumeSpec,
    pub preboot_tree: Tree,
}

pub fn unicode_for_build(build: &Build) -> UnicodeVersion {
    if build.record.ios >= 26 {
        UnicodeVersion::IOS_27
    } else {
        UnicodeVersion::IOS_17
    }
}

pub struct AssembleInput<'a> {
    pub build: &'a Build,
    pub local: &'a LocalInputs,
    pub members: &'a PreparedMembers,
    pub system_fs: &'a Fs<'a>,
    pub boot: &'a BootOutputs,
    pub deltas: Option<&'a PreparedDeltas>,
    pub time_ns: u64,
}

pub fn assemble_volumes(input: &AssembleInput<'_>, progress: &mut EngineProgress<'_>) -> Result<AssembledVolumes> {
    let build = input.build;
    let local = input.local;
    let members = input.members;
    let system_fs = input.system_fs;
    let boot = input.boot;
    let deltas = input.deltas;
    let time_ns = input.time_ns;

    progress.check_cancel()?;
    let unicode = unicode_for_build(build);
    let data_uuid = parse_uuid(DATA_VOLUME_UUID_STR)?;
    let preboot_uuid = parse_uuid(PREBOOT_VOLUME_UUID_STR)?;

    progress.log("Seeding Switch-iOS-Data from System /private/var");
    let mut data_tree = seed_data(system_fs)?.tree;

    if let Some(deltas) = deltas {
        apply_os_pretranslation(&mut data_tree, system_fs, &deltas.os_dir, progress)?;
    }
    progress.check_cancel()?;

    apply_overlay_and_apps(&mut data_tree, local, boot, progress)?;
    progress.check_cancel()?;

    apply_bootstrap(&mut data_tree, build, local, progress)?;
    progress.check_cancel()?;

    if let Some(gpu_dir) = &local.gpu_dir {
        if gpu_dir.is_file() || gpu_dir.join(switch_image_gpu::TREE_MANIFEST).is_file() {
            progress.log(format!("Staging GPU bundle from {}", gpu_dir.display()));
            let report = switch_image_gpu::stage_gpu_from_path(&mut data_tree, Some(system_fs), gpu_dir)?;
            progress.log(format!(
                "GPU bundle: {} files, {} pref plists, icon build {}",
                report.files, report.prefs, report.icon_build
            ));
        } else {
            progress.log(format!("Merging GPU bundle from {}", gpu_dir.display()));
            let gpu_src = TreeSource::from_dir(gpu_dir)?;
            let report = merge_tree(&mut data_tree, &gpu_src)?;
            progress.log(format!("GPU bundle: {} files, {} dirs", report.files, report.dirs));
        }
    }

    if let Some(seed_dir) = &local.shader_seed_dir {
        progress.log(format!("Merging shader seed from {}", seed_dir.display()));
        let seed_src = TreeSource::from_dir(seed_dir)?;
        let report = merge_tree(&mut data_tree, &seed_src)?;
        progress.log(format!("Shader seed: {} files, {} dirs", report.files, report.dirs));
    }
    progress.check_cancel()?;

    progress.log("Planning Switch-iOS-Preboot cryptexes");
    let preboot_tree = assemble_preboot(build, members, deltas, time_ns, progress)?;

    let data_spec = data_volume_spec(&DataVolumeParams::new(data_uuid, DEFAULT_DATA_NAME, unicode));
    let preboot_spec = preboot_volume_spec(&PrebootVolumeParams::new(preboot_uuid, DEFAULT_PREBOOT_NAME, unicode));

    Ok(AssembledVolumes {
        data_spec,
        data_tree,
        preboot_spec,
        preboot_tree,
    })
}

fn apply_os_pretranslation(
    data_tree: &mut Tree,
    system_fs: &Fs<'_>,
    os_delta_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    let manifest_path = os_delta_dir.join("os.manifest.json");
    let entries = load_manifest(&manifest_path)?;
    let files = DeltaDir(os_delta_dir.to_path_buf());
    let var_stats = apply_var_delta(data_tree, system_fs, &entries, &files)?;

    let tree_ents = tree_entries(&entries);
    let pairs_path = os_delta_dir.join("os.cdhash-pairs");
    let pairs_text = fs::read_to_string(&pairs_path).map_err(|e| EngineError::io_path(&pairs_path, e))?;
    let pairs = parse_cdhash_pairs(&pairs_text)?;
    let placed = place_pretranslated(data_tree, system_fs, &tree_ents, &files, &pairs)?;
    progress.log(format!(
        "Pretranslated OS: {} /private/var replaced, {} overlay binaries placed ({} dirs)",
        var_stats.replaced, placed.placed_files, placed.directories
    ));
    Ok(())
}

fn apply_overlay_and_apps(
    data_tree: &mut Tree,
    local: &LocalInputs,
    boot: &BootOutputs,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    let base = match &local.overlay_dir {
        Some(dir) => Some(TreeSource::from_dir(dir)?),
        None => None,
    };
    let merged = merge_overlay_sources(base.as_ref(), &boot.overlay_files)?;
    if merged.is_empty() {
        return Ok(());
    }

    let app_path = format!("Applications/{}", switch_image_overlay::INSTALLER_APP);
    let app_prefix = format!("{app_path}/");
    let has_full_overlay = local.overlay_dir.is_some()
        || merged
            .entries()
            .keys()
            .any(|p| p == &app_path || p.starts_with(&app_prefix));
    let builtin_pin = match &local.overlay_builtin_pin {
        Some(p) => Some(fs::read(p).map_err(|e| EngineError::io_path(p, e))?),
        None if has_full_overlay => boot.builtin_installer_pin.clone(),
        None => None,
    };
    let report = place_overlay(data_tree, &merged, builtin_pin.as_deref())?;
    progress.log(format!(
        "Overlay: {} files, {} dirs, {} symlinks",
        report.files, report.dirs, report.symlinks
    ));

    let seed_pin = local
        .overlay_seed_pin
        .as_deref()
        .map(|p| fs::read(p).map_err(|e| EngineError::io_path(p, e)))
        .transpose()?;
    let staged = stage_apps(data_tree, seed_pin.as_deref())?;
    if !staged.staged.is_empty() {
        progress.log(format!("Staged {} system apps", staged.staged.len()));
    }
    Ok(())
}

fn merge_overlay_sources(base: Option<&TreeSource>, extra: &BTreeMap<String, Vec<u8>>) -> Result<TreeSource> {
    let mut out = TreeSource::new();
    if let Some(src) = base {
        out = out.with_root_mode(src.root_mode());
        for (path, entry) in src.entries() {
            if extra.contains_key(path) {
                continue;
            }
            match &entry.kind {
                SourceKind::Dir => out.add_dir(path, entry.mode)?,
                SourceKind::File(bytes) => out.add_file(path, Arc::clone(bytes), entry.mode)?,
                SourceKind::Symlink(target) => out.add_symlink(path, target, entry.mode)?,
            }
        }
    }
    for (path, bytes) in extra {
        let mode = if path.ends_with("installd") { 0o755 } else { 0o644 };
        out.add_file(path, bytes.clone(), mode)?;
    }
    Ok(out)
}

fn apply_bootstrap(
    data_tree: &mut Tree,
    build: &Build,
    local: &LocalInputs,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    let Some(dir) = &local.bootstrap_dir else {
        return Ok(());
    };
    let pins = BootstrapPins {
        build: build.build(),
        version: &build.record.version,
    };
    let report = if dir.join("bootstrap").is_dir() {
        stage_bootstrap_from_dir(data_tree, dir, &pins)?
    } else if dir.join(MANIFEST).is_file() {
        let manifest_path = dir.join(MANIFEST);
        let archive_path = dir.join(ARCHIVE);
        let manifest_bytes = fs::read(&manifest_path).map_err(|e| EngineError::io_path(&manifest_path, e))?;
        let archive = File::open(&archive_path).map_err(|e| EngineError::io_path(&archive_path, e))?;
        let helper_path = dir.join(HELPER);
        let helper = if helper_path.is_file() {
            Some(fs::read(&helper_path).map_err(|e| EngineError::io_path(&helper_path, e))?)
        } else {
            None
        };
        Some(stage_bootstrap_reader(
            data_tree,
            &manifest_bytes,
            archive,
            helper.as_deref(),
            &pins,
        )?)
    } else {
        return Err(EngineError::Config(format!(
            "bootstrap directory {} contains neither bootstrap/ nor {MANIFEST}",
            dir.display()
        )));
    };
    if let Some(report) = report {
        progress.log(format!(
            "Bootstrap staged: {} ({}, {} entries)",
            report.build, report.firmware, report.entries
        ));
    }
    Ok(())
}

fn assemble_preboot(
    build: &Build,
    members: &PreparedMembers,
    deltas: Option<&PreparedDeltas>,
    time_ns: u64,
    progress: &mut EngineProgress<'_>,
) -> Result<Tree> {
    let vols = &build.record.volumes;
    let specs = [
        (
            Role::SystemCryptex,
            "system_cryptex",
            &members.system_cryptex,
            &vols.system_cryptex_uuid,
            &vols.system_cryptex_image,
            deltas.map(|d| d.system_cryptex_dir.as_path()),
        ),
        (
            Role::AppCryptex,
            "app_cryptex",
            &members.app_cryptex,
            &vols.app_cryptex_uuid,
            &vols.app_cryptex_image,
            deltas.map(|d| d.app_cryptex_dir.as_path()),
        ),
    ];

    let mut images = Vec::with_capacity(2);
    for (role, delta_name, path, uuid_str, stage_path, delta_dir) in specs {
        let name = staging_name(stage_path)?.to_owned();
        let content = if let Some(delta_dir) = delta_dir {
            let manifest_path = delta_dir.join(format!("{delta_name}.manifest.json"));
            let replacements = switch_apfs_seal::load_manifest(&manifest_path)?;
            let src = Arc::new(FileSource::open(path, 0).map_err(|e| EngineError::io_path(path, e))?);
            let opts = SealOptions::new(parse_uuid(uuid_str)?, time_ns);
            // Stream sealed cryptex directly into Preboot's extent without writing a temporary cryptex file.
            let fill = seal_fill(src, &replacements, &opts)?;
            progress.log(format!(
                "Planned sealed {delta_name} ({name}): {} bytes ({} replacements)",
                fill.exact_size(),
                replacements.len()
            ));
            CryptexContent::Fill {
                size: fill.exact_size(),
                fill: Arc::new(fill),
            }
        } else {
            let expected = build.container(role).map(|(_, pins)| pins.sha256.0);
            progress.log(format!("Staging verbatim {delta_name} ({name})"));
            CryptexContent::File {
                path: path.clone(),
                expected_sha256: expected,
            }
        };
        images.push(CryptexImage { name, content });
    }

    Ok(build_preboot(&images)?)
}
