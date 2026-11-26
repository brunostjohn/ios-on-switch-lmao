// SPDX-License-Identifier: GPL-3.0-or-later
//! Adds a Data volume holding `/hello` to Apple's container, written into an image file.
//! `inspect` prints a container's layer; `set-role` and `mutate` make fixtures and negative controls.

mod mutate;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::Container;
use switch_apfs_write::{
    CheckpointAreas, Content, Entry, Owner, SourcePins, Tree, VolumeSpec, WriteOptions, plan, write,
};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    AddVolume(AddVolume),
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
    /// `keep`, `resized` (280/27,748), or `DESC:DATA` blocks.
    #[arg(long, value_parser = parse_areas)]
    areas: CheckpointAreas,
    #[arg(long)]
    time_ns: u64,
    #[arg(long, default_value = "hello\n")]
    hello: String,
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_apfs_m1: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> Result<(), Box<dyn std::error::Error>> {
    match cli.command {
        Command::AddVolume(a) => add_volume(a),
        Command::Inspect { image, offset } => inspect(&image, offset),
        Command::SetRole { image, uuid, role } => mutate::set_role(&image, uuid, role),
        Command::Mutate { image, what, uuid } => mutate::mutate(&image, what, uuid),
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
    let tree = Tree {
        root: Owner {
            uid: 0,
            gid: 0,
            mode: 0o755,
        },
        entries: vec![Entry {
            name: "hello".into(),
            owner: Owner {
                uid: 0,
                gid: 0,
                mode: 0o644,
            },
            content: Content::File(a.hello.into_bytes()),
        }],
    };
    let options = WriteOptions {
        target_blocks: a.target_bytes / switch_apfs_format::BLOCK_SIZE as u64,
        areas: a.areas,
        time_ns: a.time_ns,
        formatted_by: "switch_apfs (m1)".into(),
    };
    let started = std::time::Instant::now();
    let plan = plan(
        &source,
        &SourcePins {
            system_uuid: a.system_uuid,
        },
        &spec,
        &tree,
        &options,
    )?;
    let planned = started.elapsed();
    let mut target = FileTarget::create(&a.target, a.target_bytes)?;
    write(&source, &mut target, &plan)?;
    let s = &plan.summary;
    println!("source: xid {} blocks {}", s.source_xid, s.source_blocks);
    println!(
        "queued frees: {} blocks freed, {} already clear",
        s.queued_freed, s.queued_already_free
    );
    println!("container layer freed: {} blocks", s.container_freed);
    println!(
        "target: {} blocks, xid {}, {} free",
        plan.target_blocks, plan.xid, s.free_blocks
    );
    println!("checkpoint areas: desc {:?} data {:?}", plan.desc_area, plan.data_area);
    println!("pool {:?} ring {:?}", s.pool, s.pool_bitmap_ring);
    println!(
        "volume: slot {} oid {} superblock at {}",
        s.volume_index, s.volume_oid, s.volume_superblock
    );
    println!(
        "copied {} blocks; planned in {planned:?}, done in {:?}",
        s.copied_blocks,
        started.elapsed()
    );
    Ok(())
}

fn inspect(image: &PathBuf, offset: u64) -> Result<(), Box<dyn std::error::Error>> {
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

pub fn uuid_string(u: &[u8; 16]) -> String {
    let h: String = u.iter().map(|b| format!("{b:02X}")).collect();
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}

fn parse_uuid(s: &str) -> Result<[u8; 16], String> {
    let hex: String = s.chars().filter(|c| *c != '-').collect();
    if hex.len() != 32 {
        return Err(format!("not a UUID: {s}"));
    }
    let mut out = [0u8; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|_| format!("not a UUID: {s}"))?;
    }
    Ok(out)
}

fn parse_int<T: TryFrom<u64>>(s: &str) -> Result<T, String> {
    let v = match s.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16),
        None => s.parse(),
    }
    .map_err(|_| format!("not a number: {s}"))?;
    T::try_from(v).map_err(|_| format!("out of range: {s}"))
}

fn parse_areas(s: &str) -> Result<CheckpointAreas, String> {
    match s {
        "keep" => Ok(CheckpointAreas::Keep),
        "resized" => Ok(CheckpointAreas::RESIZED),
        _ => {
            let (desc, data) = s
                .split_once(':')
                .ok_or_else(|| format!("not keep, resized or DESC:DATA: {s}"))?;
            Ok(CheckpointAreas::Sized {
                desc: parse_int(desc)?,
                data: parse_int(data)?,
            })
        }
    }
}
