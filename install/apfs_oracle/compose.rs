// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use switch_apfs_alloc::Geometry;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_seal::{SealOptions, load_manifest as load_seal_manifest, seal_fill};
use switch_apfs_write::{CheckpointAreas, NewVolume, Plan, SourcePins, Tree, VolumeSpec, WriteOptions, plan, write};
use switch_image::{
    CryptexContent, CryptexImage, DataVolumeParams, DeltaDir, PrebootVolumeParams, apply_var_delta, build_preboot,
    data_volume_spec, load_manifest, preboot_volume_spec, seed_data, tree_entries,
};
use switch_image_bootstrap::{ARCHIVE, BootstrapPins, MANIFEST, stage_bootstrap_reader};
use switch_image_overlay::{TreeSource, merge_tree, place_overlay, stage_apps};
use switch_image_pretranslated::{parse_cdhash_pairs, place_pretranslated};

use crate::parse::{SealedCryptexSpec, VerbatimCryptexSpec};
use crate::{Error, Result, plan_dump};

#[derive(Clone, Debug)]
pub struct ComposeConfig {
    pub source: PathBuf,
    pub source_offset: u64,
    pub target_bytes: u64,
    pub system_uuid: [u8; 16],
    pub data_uuid: [u8; 16],
    pub data_name: String,
    pub preboot_uuid: [u8; 16],
    pub preboot_name: String,
    pub unicode: UnicodeVersion,
    pub time_ns: u64,
    pub areas: CheckpointAreas,
    pub formatted_by: String,
    pub delta_dir: Option<PathBuf>,
    pub manifest_out: Option<PathBuf>,
    pub overlay_dir: Option<PathBuf>,
    pub builtin_pin: Option<PathBuf>,
    pub seed_pin: Option<PathBuf>,
    pub bootstrap_dir: Option<PathBuf>,
    pub bootstrap_helper: Option<PathBuf>,
    pub build: Option<String>,
    pub firmware: Option<String>,
    pub shader_seed_dir: Option<PathBuf>,
    pub sealed_cryptexes: Vec<SealedCryptexSpec>,
    pub verbatim_cryptexes: Vec<VerbatimCryptexSpec>,
    pub expected_data_dump: Option<PathBuf>,
    pub expected_preboot_dump: Option<PathBuf>,
}

pub struct BuiltTrees {
    pub data: Tree,
    pub preboot: Tree,
    pub data_spec: VolumeSpec,
    pub preboot_spec: VolumeSpec,
    pub var_replaced: usize,
    pub pretranslated_placed: usize,
    pub staged_apps: usize,
    pub bootstrap_entries: usize,
    pub shader_files: usize,
    pub sealed_cryptex_count: usize,
    pub verbatim_cryptex_count: usize,
}

impl ComposeConfig {
    pub fn write_options(&self) -> Result<WriteOptions> {
        if !self.target_bytes.is_multiple_of(BLOCK_SIZE as u64) || self.target_bytes == 0 {
            return Err(Error::Check(format!(
                "target_bytes {} is not a positive multiple of {BLOCK_SIZE}",
                self.target_bytes
            )));
        }
        Ok(WriteOptions {
            target_blocks: self.target_bytes / BLOCK_SIZE as u64,
            areas: self.areas,
            time_ns: self.time_ns,
            formatted_by: self.formatted_by.clone(),
        })
    }

    pub fn source_pins(&self) -> SourcePins {
        SourcePins {
            system_uuid: self.system_uuid,
        }
    }
}

pub fn build_trees(cfg: &ComposeConfig, src: &dyn BlockSource) -> Result<BuiltTrees> {
    let container = Container::open(src)?;
    let system_vol = container
        .volume(cfg.system_uuid)
        .ok_or_else(|| Error::Check("source has no volume matching system_uuid".into()))?;
    let system_fs = Fs::open(src, system_vol)?;

    let mut data = seed_data(&system_fs)?.tree;
    let mut var_replaced = 0usize;
    let mut pretranslated_placed = 0usize;
    let mut staged_apps = 0usize;
    let mut bootstrap_entries = 0usize;
    let mut shader_files = 0usize;

    let delta_loaded = match &cfg.delta_dir {
        Some(dir) => {
            let entries = load_manifest(&dir.join("os.manifest.json"))?;
            let loader = DeltaDir(dir.clone());
            let stats = apply_var_delta(&mut data, &system_fs, &entries, &loader)?;
            var_replaced = stats.replaced;
            Some((dir, entries, loader))
        }
        None => None,
    };

    if let Some(overlay_dir) = &cfg.overlay_dir {
        let overlay_src = TreeSource::from_dir(overlay_dir)?;
        let builtin = read_opt_file(cfg.builtin_pin.as_deref())?;
        place_overlay(&mut data, &overlay_src, builtin.as_deref())?;
        let seed_pin = read_opt_file(cfg.seed_pin.as_deref())?;
        let app_report = stage_apps(&mut data, seed_pin.as_deref())?;
        staged_apps = app_report.staged.len();
    }

    if let Some(bs_dir) = &cfg.bootstrap_dir {
        let build = cfg
            .build
            .as_deref()
            .ok_or_else(|| Error::Check("--build is required with --bootstrap-dir".into()))?;
        let version = cfg
            .firmware
            .as_deref()
            .ok_or_else(|| Error::Check("--firmware is required with --bootstrap-dir".into()))?;
        let dir = if bs_dir.join(MANIFEST).is_file() {
            bs_dir.clone()
        } else {
            bs_dir.join("bootstrap")
        };
        let manifest_bytes = fs::read(dir.join(MANIFEST))?;
        let archive_file = File::open(dir.join(ARCHIVE))?;
        let helper_bytes = read_opt_file(cfg.bootstrap_helper.as_deref())?;
        let pins = BootstrapPins { build, version };
        let report = stage_bootstrap_reader(&mut data, &manifest_bytes, archive_file, helper_bytes.as_deref(), &pins)?;
        bootstrap_entries = report.entries;
    }

    if let Some(seed_dir) = &cfg.shader_seed_dir {
        let seed_src = TreeSource::from_dir(seed_dir)?;
        let merge_report = merge_tree(&mut data, &seed_src)?;
        shader_files = merge_report.files;
    }

    if let Some((delta_dir, entries, loader)) = delta_loaded {
        let pairs_path = delta_dir.join("os.cdhash-pairs");
        let pairs = if pairs_path.is_file() {
            parse_cdhash_pairs(&fs::read_to_string(&pairs_path)?)?
        } else {
            Vec::new()
        };
        let placed = place_pretranslated(&mut data, &system_fs, &tree_entries(&entries), &loader, &pairs)?;
        pretranslated_placed = placed.placed_files;
        if let Some(out) = &cfg.manifest_out {
            fs::write(out, &placed.manifest_json)?;
        }
    }

    let mut cryptex_images = Vec::with_capacity(cfg.sealed_cryptexes.len() + cfg.verbatim_cryptexes.len());
    for spec in &cfg.sealed_cryptexes {
        let c_src = Arc::new(FileSource::open(&spec.source, spec.offset)?);
        let replacements = match &spec.manifest {
            Some(path) => load_seal_manifest(path)?,
            None => Vec::new(),
        };
        let seal_opts = SealOptions {
            volume_uuid: spec.uuid,
            volume_name: spec.vol_name.clone(),
            time_ns: cfg.time_ns,
            formatted_by: cfg.formatted_by.clone(),
        };
        let fill = seal_fill(c_src, &replacements, &seal_opts)?;
        cryptex_images.push(CryptexImage {
            name: spec.name.clone(),
            content: CryptexContent::Fill {
                size: fill.exact_size(),
                fill: Arc::new(fill),
            },
        });
    }
    for spec in &cfg.verbatim_cryptexes {
        cryptex_images.push(CryptexImage {
            name: spec.name.clone(),
            content: CryptexContent::File {
                path: spec.path.clone(),
                expected_sha256: spec.sha256,
            },
        });
    }
    let preboot = build_preboot(&cryptex_images)?;

    let data_spec = data_volume_spec(&DataVolumeParams::new(cfg.data_uuid, &cfg.data_name, cfg.unicode));
    let preboot_spec = preboot_volume_spec(&PrebootVolumeParams::new(
        cfg.preboot_uuid,
        &cfg.preboot_name,
        cfg.unicode,
    ));

    Ok(BuiltTrees {
        data,
        preboot,
        data_spec,
        preboot_spec,
        var_replaced,
        pretranslated_placed,
        staged_apps,
        bootstrap_entries,
        shader_files,
        sealed_cryptex_count: cfg.sealed_cryptexes.len(),
        verbatim_cryptex_count: cfg.verbatim_cryptexes.len(),
    })
}

pub fn plan_trees(
    cfg: &ComposeConfig,
    src: &dyn BlockSource,
    trees: &BuiltTrees,
    stale: &[Range<u64>],
) -> Result<Plan> {
    let options = cfg.write_options()?;
    let pins = cfg.source_pins();
    Ok(plan(
        src,
        &pins,
        &[
            NewVolume {
                spec: &trees.data_spec,
                tree: &trees.data,
            },
            NewVolume {
                spec: &trees.preboot_spec,
                tree: &trees.preboot,
            },
        ],
        &options,
        stale,
    )?)
}

pub fn compose(cfg: &ComposeConfig, target_path: &Path) -> Result<()> {
    let src = FileSource::open(&cfg.source, cfg.source_offset)?;
    let trees = build_trees(cfg, &src)?;
    let planned = plan_trees(cfg, &src, &trees, &[])?;

    if let Some(path) = &cfg.expected_data_dump {
        let dump = plan_dump::dump_tree(&trees.data, &src, cfg.time_ns, true)?;
        plan_dump::write_json(path, &dump)?;
    }
    if let Some(path) = &cfg.expected_preboot_dump {
        let dump = plan_dump::dump_tree(&trees.preboot, &src, cfg.time_ns, false)?;
        plan_dump::write_json(path, &dump)?;
    }

    let mut target = FileTarget::create(target_path, cfg.target_bytes)?;
    let stats = write(&src, &mut target, &planned)?;
    let g = Geometry::for_blocks(planned.target_blocks)?;

    println!(
        "compose: blocks={} pool_base={} pool_blocks={} free={} xid={}",
        g.blocks, planned.summary.pool.start, g.pool_blocks, planned.summary.free_blocks, planned.xid,
    );
    println!(
        "data: var_replaced={} pretranslated={} staged_apps={} bootstrap={} shader_files={}",
        trees.var_replaced, trees.pretranslated_placed, trees.staged_apps, trees.bootstrap_entries, trees.shader_files,
    );
    println!(
        "preboot: sealed_cryptexes={} verbatim_cryptexes={}",
        trees.sealed_cryptex_count, trees.verbatim_cryptex_count,
    );
    let total_writes: u64 = stats.iter().map(|s| s.writes).sum();
    let total_bytes: u64 = stats.iter().map(|s| s.bytes).sum();
    println!("writes: {total_writes} requests, {total_bytes} bytes");
    Ok(())
}

fn read_opt_file(path: Option<&Path>) -> Result<Option<Vec<u8>>> {
    match path {
        Some(p) => Ok(Some(fs::read(p)?)),
        None => Ok(None),
    }
}
