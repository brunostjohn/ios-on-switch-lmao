// SPDX-License-Identifier: GPL-3.0-or-later
//! The APFS writer's driver, on image files:
//! - `build` adds Data, a verbatim copy of System's `/private/var`, and Preboot with seeded images;
//! - `power-cuts` cuts `build`'s write at every phase and checks what is left, in memory or on image files;
//! - `fidelity` compares a copy with its source record for record, for what a mount can't show;
//! - `add-volume` adds a Data volume holding only `/hello` (M1);
//! - `source` turns an IPSW's OS member into the bare container the writer reads;
//! - `gen` prints a seeded image's bytes; `inspect` prints a container's layer;
//! - `set-role` and `mutate` make fixtures and negative controls.

mod build;
mod cut_plan;
mod cuts;
mod fidelity;
mod file_cuts;
mod mutate;
mod parse;
mod samples;
mod seeded;
mod sharing;
mod source;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_format::object::{OBJECT_TYPE_MASK, TYPE_NX_REAP_LIST, TYPE_NX_REAPER};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::Container;
use switch_apfs_write::{Attrs, CheckpointAreas, NewVolume, SourcePins, Tree, VolumeSpec, WriteOptions, plan, write};

use crate::parse::{parse_areas, parse_int, parse_uuid, uuid_string};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Build {
        #[command(flatten)]
        args: build::BuildArgs,
        #[arg(long)]
        target: PathBuf,
    },
    PowerCuts(cuts::CutArgs),
    Fidelity(fidelity::FidelityArgs),
    AddVolume(AddVolume),
    Source(source::SourceArgs),
    Gen {
        #[arg(long, value_parser = parse_int::<u64>)]
        bytes: u64,
        #[arg(long, value_parser = parse_int::<u64>)]
        seed: u64,
    },
    Inspect {
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
    },
    SetRole {
        image: PathBuf,
        #[arg(long, value_parser = parse_uuid)]
        uuid: [u8; 16],
        #[arg(long, value_parser = parse_int::<u16>)]
        role: u16,
    },
    Mutate {
        image: PathBuf,
        #[arg(value_enum)]
        what: mutate::Mutation,
        #[arg(long, value_parser = parse_uuid)]
        uuid: Option<[u8; 16]>,
    },
}

#[derive(clap::Args)]
struct AddVolume {
    #[arg(long)]
    source: PathBuf,
    #[arg(long, default_value_t = 0)]
    source_offset: u64,
    #[arg(long)]
    target: PathBuf,
    #[arg(long)]
    target_bytes: u64,
    #[arg(long, value_parser = parse_uuid)]
    system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid)]
    uuid: [u8; 16],
    #[arg(long)]
    name: String,
    #[arg(long, value_parser = parse_int::<u16>)]
    role: u16,
    #[arg(long, value_parser = parse_int::<u64>)]
    features: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    rocompat: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    incompat: u64,
    #[arg(long)]
    unicode: UnicodeVersion,
    /// `rule` (the default), `keep`, `mac-composed` (280/27,748), or `DESC:DATA` blocks.
    #[arg(long, value_parser = parse_areas, default_value = "rule")]
    areas: CheckpointAreas,
    #[arg(long)]
    time_ns: u64,
    #[arg(long, default_value = "hello\n")]
    hello: String,
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("switch_apfs_m1: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> Result<bool, Box<dyn std::error::Error>> {
    match cli.command {
        Command::Build { args, target } => build::build(&args, &target).map(|()| true),
        Command::PowerCuts(args) => cuts::run(&args),
        Command::Fidelity(args) => fidelity::run(&args),
        Command::AddVolume(a) => add_volume(a).map(|()| true),
        Command::Source(a) => source::run(&a).map(|()| true),
        Command::Gen { bytes, seed } => seeded::to_stdout(seed, bytes).map(|()| true).map_err(Into::into),
        Command::Inspect { image, offset } => inspect(&image, offset).map(|()| true),
        Command::SetRole { image, uuid, role } => mutate::set_role(&image, uuid, role).map(|()| true),
        Command::Mutate { image, what, uuid } => mutate::mutate(&image, what, uuid).map(|()| true),
    }
}

fn add_volume(a: AddVolume) -> Result<(), Box<dyn std::error::Error>> {
    let source = FileSource::open(&a.source, a.source_offset)?;
    let spec = VolumeSpec {
        uuid: a.uuid,
        name: a.name,
        role: a.role,
        features: a.features,
        readonly_compatible_features: a.rocompat,
        incompatible_features: a.incompat,
        unicode: a.unicode,
    };
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let hello = tree.add_bytes(a.hello.into_bytes());
    tree.add_file(tree.root(), "hello", Attrs::new(0, 0, 0o644), Some(hello))?;
    let options = WriteOptions {
        target_blocks: a.target_bytes / switch_apfs_format::BLOCK_SIZE as u64,
        areas: a.areas,
        time_ns: a.time_ns,
        formatted_by: "switch_apfs (m1)".into(),
    };
    let started = std::time::Instant::now();
    let pins = SourcePins {
        system_uuid: a.system_uuid,
    };
    let plan = plan(
        &source,
        &pins,
        &[NewVolume {
            spec: &spec,
            tree: &tree,
        }],
        &options,
        &[],
    )?;
    let planned = started.elapsed();
    let mut target = FileTarget::create(&a.target, a.target_bytes)?;
    let stats = write(&source, &mut target, &plan)?;
    build::summary(&plan);
    build::print_stats(&stats);
    println!("planned in {planned:?}, done in {:?}", started.elapsed());
    Ok(())
}

fn inspect(image: &Path, offset: u64) -> Result<(), Box<dyn std::error::Error>> {
    let c = Container::open(&FileSource::open(image, offset)?)?;
    let cp = &c.checkpoint;
    println!(
        "xid {} blocks {} uuid {}",
        cp.xid(),
        c.blocks(),
        uuid_string(&cp.nx.uuid)
    );
    println!(
        "desc {:?} data {:?} max_fs {}",
        cp.desc_area,
        cp.data_area,
        cp.nx.max_file_systems.get()
    );
    let sm = &c.space.spaceman;
    println!(
        "free {} pool {:?} ring {:?} queue limits {}/{}",
        c.space.free_count,
        c.space.pool,
        c.space.pool_bitmap_ring,
        sm.fq[0].tree_node_limit.get(),
        sm.fq[1].tree_node_limit.get()
    );
    for m in &cp.mappings {
        let kind = m.type_and_flags.get() & OBJECT_TYPE_MASK;
        if kind != TYPE_NX_REAPER && kind != TYPE_NX_REAP_LIST {
            continue;
        }
        let (_, block) = cp.ephemeral(m.oid.get())?;
        let u32_at = |at: usize| u32::from_le_bytes([block[at], block[at + 1], block[at + 2], block[at + 3]]);
        let u64_at = |at: usize| u64::from(u32_at(at)) | u64::from(u32_at(at + 4)) << 32;
        if kind == TYPE_NX_REAPER {
            println!(
                "reaper {}: next reap id {} completed {} lists {:#x}..{:#x} flags {:#x} count {} reaping fs {:#x} oid {:#x}",
                m.oid.get(),
                u64_at(32),
                u64_at(40),
                u64_at(48),
                u64_at(56),
                u32_at(64),
                u32_at(68),
                u64_at(80),
                u64_at(88)
            );
        } else {
            println!(
                "reap list {}: next {:#x} flags {:#x} max {} count {} first {:#x} last {:#x} free {:#x}",
                m.oid.get(),
                u64_at(32),
                u32_at(40),
                u32_at(44),
                u32_at(48),
                u32_at(52),
                u32_at(56),
                u32_at(60)
            );
        }
    }
    for (q, entries) in c.space.queues.iter().enumerate() {
        println!(
            "queue {q}: {} entries, {} blocks",
            entries.len(),
            entries.iter().map(|e| e.len).sum::<u64>()
        );
    }
    for v in &c.volumes {
        let sb = &v.sb;
        println!(
            "volume {} oid {} xid {} at {}: {} uuid {} role {:#x} features {:#x} rocompat {:#x} incompat {:#x}",
            v.index,
            v.oid,
            v.xid,
            v.paddr,
            sb.name(),
            uuid_string(&sb.vol_uuid),
            sb.role.get(),
            sb.features.get(),
            sb.readonly_compatible_features.get(),
            sb.incompatible_features.get()
        );
    }
    Ok(())
}
