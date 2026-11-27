// SPDX-License-Identifier: GPL-3.0-or-later
//! The B-tree builder against Apple's trees and the kernel, on a Mac:
//!
//! - `make-tree` writes the standard tree into a mounted volume, so macOS builds its own trees for it;
//! - `compare` rebuilds an image's trees from their records and compares them with Apple's, node for node;
//! - `add-volume` adds a Data volume holding the standard tree to a container, through M1's container layer;
//! - `verify` reads the standard tree back through a mount;
//! - `last-fs-leaf` names a block for the negative control.

mod compare;
mod nodes;
mod standard;
mod verify;

use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_write::{CheckpointAreas, NewVolume, SourcePins, VolumeSpec, WriteOptions, plan, write};

use crate::standard::Spec;

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Args)]
struct Size {
    #[arg(long)]
    dirs: usize,
    #[arg(long)]
    files: usize,
}

impl Size {
    fn spec(&self) -> Spec {
        Spec {
            dirs: self.dirs,
            files: self.files,
        }
    }
}

#[derive(Subcommand)]
enum Command {
    MakeTree {
        mount: PathBuf,
        #[command(flatten)]
        size: Size,
    },
    Compare {
        image: PathBuf,
        #[arg(long)]
        volume: Option<String>,
        /// Trees that must be rebuilt byte for byte: omap, fs, extentref, fext.
        #[arg(long, value_delimiter = ',')]
        require: Vec<String>,
    },
    AddVolume(AddVolume),
    Verify {
        mount: PathBuf,
        #[command(flatten)]
        size: Size,
    },
    LastFsLeaf {
        image: PathBuf,
        volume: String,
    },
}

#[derive(Args)]
struct AddVolume {
    #[arg(long)]
    source: PathBuf,
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
    #[arg(long)]
    time_ns: u64,
    #[command(flatten)]
    size: Size,
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("switch_apfs_btree_check: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> Result<bool, Box<dyn Error>> {
    match cli.command {
        Command::MakeTree { mount, size } => make_tree(&mount, &size.spec()).map(|()| true),
        Command::Compare { image, volume, require } => Ok(compare::run(&image, volume.as_deref(), &require)?),
        Command::AddVolume(a) => add_volume(a).map(|()| true),
        Command::Verify { mount, size } => Ok(verify::run(&mount, &size.spec())?),
        Command::LastFsLeaf { image, volume } => {
            println!("{}", compare::last_fs_leaf(&image, &volume)?);
            Ok(true)
        }
    }
}

fn make_tree(mount: &std::path::Path, spec: &Spec) -> Result<(), Box<dyn Error>> {
    let (dirs, files) = standard::listing(spec);
    for dir in &dirs {
        fs::create_dir(mount.join(&dir.name))?;
    }
    for file in &files {
        fs::write(mount.join(&file.dir).join(&file.name), &file.data)?;
    }
    println!("made {} directories and {} files", dirs.len(), files.len());
    Ok(())
}

/// The Data volume's pins are M1's: role, feature bits and Unicode 17.0.
fn add_volume(a: AddVolume) -> Result<(), Box<dyn Error>> {
    let source = FileSource::open(&a.source, 0)?;
    let spec = VolumeSpec {
        uuid: a.uuid,
        name: a.name,
        role: 0x40,
        features: 0x2,
        readonly_compatible_features: 0x0C,
        incompatible_features: 0x200,
        unicode: UnicodeVersion::IOS_27,
    };
    let options = WriteOptions {
        target_blocks: a.target_bytes / switch_apfs_format::BLOCK_SIZE as u64,
        areas: CheckpointAreas::MAC_COMPOSED,
        time_ns: a.time_ns,
        formatted_by: "switch_apfs (w2)".into(),
    };
    let started = std::time::Instant::now();
    let pins = SourcePins {
        system_uuid: a.system_uuid,
    };
    let tree = standard::tree(&a.size.spec());
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
    write(&source, &mut target, &plan)?;
    let data = &plan.summary.volumes[0].stats;
    println!(
        "Data: fs tree {} nodes / {} levels, extent-reference tree {}, object map {}, {} data blocks; \
         planned in {planned:?}, written in {:?}",
        data.fs_nodes,
        data.fs_levels,
        data.extent_ref_nodes,
        data.omap_nodes,
        data.data_blocks,
        started.elapsed() - planned
    );
    Ok(())
}

fn parse_uuid(s: &str) -> Result<[u8; 16], String> {
    let hex: String = s.chars().filter(|c| *c != '-').collect();
    if hex.len() != 32 {
        return Err(format!("not a UUID: {s}"));
    }
    let mut out = [0u8; 16];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|_| format!("not a UUID: {s}"))?;
    }
    Ok(out)
}
