// SPDX-License-Identifier: GPL-3.0-or-later
//! `build`: Data with a verbatim copy of System's subtree and Preboot with seeded stand-in images, written into
//! Apple's container in an image file.

use std::ops::Range;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::{
    Attrs, CheckpointAreas, Content, Copied, NewVolume, PhaseStats, Plan, SourcePins, Tree, VolumeSpec, WriteOptions,
    copy_subtree, plan, write,
};

use crate::parse::{parse_areas, parse_file, parse_int, parse_uuid};
use crate::samples::{self, Sample};
use crate::seeded;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(Clone, Debug)]
pub struct GenFile {
    pub name: String,
    pub bytes: u64,
    pub seed: u64,
}

#[derive(clap::Args, Clone)]
pub struct BuildArgs {
    #[arg(long)]
    pub source: PathBuf,
    #[arg(long, default_value_t = 0)]
    pub source_offset: u64,
    #[arg(long)]
    pub target_bytes: u64,
    #[arg(long, value_parser = parse_uuid)]
    pub system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid)]
    pub data_uuid: [u8; 16],
    #[arg(long)]
    pub data_name: String,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub data_features: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub data_rocompat: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub data_incompat: u64,
    #[arg(long, value_parser = parse_uuid)]
    pub preboot_uuid: [u8; 16],
    #[arg(long)]
    pub preboot_name: String,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub preboot_features: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub preboot_rocompat: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    pub preboot_incompat: u64,
    #[arg(long)]
    pub unicode: UnicodeVersion,
    /// `rule` (the default), `keep`, `mac-composed` (280/27,748), or `DESC:DATA` blocks.
    #[arg(long, value_parser = parse_areas, default_value = "rule")]
    pub areas: CheckpointAreas,
    #[arg(long)]
    pub time_ns: u64,
    #[arg(long, default_value = "switch_apfs")]
    pub formatted_by: String,
    /// The source subtree Data's root holds.
    #[arg(long, default_value = "/private/var")]
    pub copy: String,
    /// `NAME:BYTES:SEED`: a seeded file in Preboot's Switch-Staging, one contiguous extent.
    #[arg(long = "preboot-file", value_parser = parse_file)]
    pub preboot_files: Vec<GenFile>,
    #[arg(long = "sample", value_enum)]
    pub samples: Vec<Sample>,
}

impl BuildArgs {
    pub fn options(&self) -> WriteOptions {
        WriteOptions {
            target_blocks: self.target_bytes / BLOCK_SIZE as u64,
            areas: self.areas,
            time_ns: self.time_ns,
            formatted_by: self.formatted_by.clone(),
        }
    }

    fn specs(&self) -> [VolumeSpec; 2] {
        [
            VolumeSpec {
                uuid: self.data_uuid,
                name: self.data_name.clone(),
                role: 0x40,
                features: self.data_features,
                readonly_compatible_features: self.data_rocompat,
                incompatible_features: self.data_incompat,
                unicode: self.unicode,
            },
            VolumeSpec {
                uuid: self.preboot_uuid,
                name: self.preboot_name.clone(),
                role: 0x10,
                features: self.preboot_features,
                readonly_compatible_features: self.preboot_rocompat,
                incompatible_features: self.preboot_incompat,
                unicode: self.unicode,
            },
        ]
    }
}

pub struct Trees {
    pub specs: [VolumeSpec; 2],
    pub data: Tree,
    pub preboot: Tree,
    pub copied: Copied,
}

impl Trees {
    pub fn volumes(&self) -> [NewVolume<'_>; 2] {
        [
            NewVolume {
                spec: &self.specs[0],
                tree: &self.data,
            },
            NewVolume {
                spec: &self.specs[1],
                tree: &self.preboot,
            },
        ]
    }
}

pub fn trees(args: &BuildArgs, src: &dyn BlockSource) -> Result<Trees> {
    let c = Container::open(src)?;
    let system = c
        .volume(args.system_uuid)
        .ok_or("the source has no volume with the System UUID")?;
    let fs = Fs::open(src, system)?;
    let mut data = Tree::new(Attrs::new(0, 0, 0o755));
    let root = data.root();
    let copied = copy_subtree(&fs, &args.copy, &mut data, root)?;
    let mut preboot = Tree::new(Attrs::new(0, 0, 0o755));
    let staging = preboot.add_dir(preboot.root(), "Switch-Staging", Attrs::new(0, 0, 0o755))?;
    for f in &args.preboot_files {
        let stream = preboot.add_stream(f.bytes, Content::Fill(Arc::new(seeded::fill(f.seed, f.bytes))));
        preboot.add_file(staging, &f.name, Attrs::new(0, 0, 0o644), Some(stream))?;
    }
    for &sample in &args.samples {
        samples::add(sample, &mut data, &mut preboot)?;
    }
    Ok(Trees {
        specs: args.specs(),
        data,
        preboot,
        copied,
    })
}

/// `stale`: the older container's descriptor areas, read from the target before its first phase B.
pub fn plan_for(args: &BuildArgs, src: &dyn BlockSource, trees: &Trees, stale: &[Range<u64>]) -> Result<Plan> {
    let pins = SourcePins {
        system_uuid: args.system_uuid,
    };
    Ok(plan(src, &pins, &trees.volumes(), &args.options(), stale)?)
}

pub fn build(args: &BuildArgs, target: &Path) -> Result<()> {
    let src = FileSource::open(&args.source, args.source_offset)?;
    let started = Instant::now();
    let trees = trees(args, &src)?;
    let copied_at = started.elapsed();
    let plan = plan_for(args, &src, &trees, &[])?;
    let planned = started.elapsed();
    let mut out = FileTarget::create(target, args.target_bytes)?;
    let stats = write(&src, &mut out, &plan)?;
    let c = &trees.copied;
    println!(
        "copy of {}: {} entries, {} inodes, {} streams ({} shared), {} hard-link names outside it",
        args.copy,
        c.entries,
        c.inodes.len(),
        c.streams,
        c.shared_streams,
        c.names_outside
    );
    summary(&plan);
    print_stats(&stats);
    println!(
        "walked in {copied_at:.2?}, planned in {:.2?}, written in {:.2?}, total {:.2?}",
        planned - copied_at,
        started.elapsed() - planned,
        started.elapsed()
    );
    Ok(())
}

pub fn summary(plan: &Plan) {
    let s = &plan.summary;
    println!("source: xid {} blocks {}", s.source_xid, s.source_blocks);
    println!(
        "queued frees: {} blocks freed, {} already clear; container layer freed {} blocks",
        s.queued_freed, s.queued_already_free, s.container_freed
    );
    println!(
        "target: {} blocks, xid {}, {} free; checkpoint areas desc {:?} data {:?}; pool {:?} ring {:?}",
        plan.target_blocks, plan.xid, s.free_blocks, plan.desc_area, plan.data_area, s.pool, s.pool_bitmap_ring
    );
    println!(
        "kept {} source blocks; {} of them also copied to new volumes; {} blocks of new data",
        s.copied_blocks, s.relocated_blocks, s.new_data_blocks
    );
    for v in &s.volumes {
        let st = &v.stats;
        println!(
            "volume slot {} oid {} at {}: {} files {} dirs {} symlinks; fs tree {} nodes / {} levels, extent refs {}, \
             omap {}; data {} blocks ({} shared), {} clone mappings",
            v.index,
            v.oid,
            st.superblock,
            st.files,
            st.dirs,
            st.symlinks,
            st.fs_nodes,
            st.fs_levels,
            st.extent_ref_nodes,
            st.omap_nodes,
            st.data_blocks,
            st.shared_blocks,
            st.clone_mappings
        );
    }
}

pub fn print_stats(stats: &[PhaseStats]) {
    for p in stats {
        println!(
            "phase {:?}: {} writes, {:.1} MB in {:.2?}, {:.0} MB/s",
            p.phase,
            p.writes,
            p.bytes as f64 / 1e6,
            p.elapsed,
            p.mb_per_s()
        );
    }
}
