// SPDX-License-Identifier: GPL-3.0-or-later
//! End-to-end driver applying `switch_image_overlay` and `switch_image_bootstrap` onto an APFS Data `Tree` and
//! committing it into Apple's container via `switch_apfs_write`.

use std::fs;
use std::path::PathBuf;

use clap::Parser;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::{
    Attrs, CheckpointAreas, NewVolume, SourcePins, Tree, VolumeSpec, WriteOptions, copy_subtree, plan, write,
};
use switch_image_bootstrap::{BootstrapPins, stage_bootstrap_from_dir};
use switch_image_overlay::{TreeSource, merge_tree, place_overlay, stage_apps};

#[derive(Parser)]
struct Args {
    #[arg(long)]
    source: PathBuf,
    #[arg(long)]
    target: PathBuf,
    #[arg(long)]
    target_bytes: u64,
    #[arg(long, value_parser = parse_uuid)]
    system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid)]
    data_uuid: [u8; 16],
    #[arg(long, default_value = "SwitchDataTrial")]
    data_name: String,
    #[arg(long)]
    overlay: PathBuf,
    #[arg(long)]
    builtin_pin: Option<PathBuf>,
    #[arg(long)]
    seed_pin: Option<PathBuf>,
    #[arg(long)]
    bootstrap_package: PathBuf,
    #[arg(long)]
    shader_seed: Option<PathBuf>,
    #[arg(long)]
    build: String,
    #[arg(long)]
    firmware: String,
    #[arg(long, default_value_t = 1_795_000_000_000_000_000)]
    time_ns: u64,
}

fn parse_uuid(s: &str) -> Result<[u8; 16], String> {
    let hex_str: String = s.chars().filter(|c| *c != '-').collect();
    if hex_str.len() != 32 {
        return Err(format!("invalid UUID {s}"));
    }
    let mut out = [0u8; 16];
    for (i, chunk) in hex_str.as_bytes().chunks_exact(2).enumerate() {
        let pair = std::str::from_utf8(chunk).map_err(|e| e.to_string())?;
        out[i] = u8::from_str_radix(pair, 16).map_err(|e| e.to_string())?;
    }
    Ok(out)
}

fn run(args: Args) -> Result<(), Box<dyn std::error::Error>> {
    let src = FileSource::open(&args.source, 0)?;
    let container = Container::open(&src)?;
    let system = container
        .volume(args.system_uuid)
        .ok_or("source has no volume with the System UUID")?;
    let fs = Fs::open(&src, system)?;

    let mut data = Tree::new(Attrs::new(0, 0, 0o755));
    let root = data.root();
    copy_subtree(&fs, "/private/var", &mut data, root)?;

    let overlay_src = TreeSource::from_dir(&args.overlay)?;
    let builtin_pin = args.builtin_pin.as_deref().map(fs::read).transpose()?;
    let overlay_report = place_overlay(&mut data, &overlay_src, builtin_pin.as_deref())?;

    let seed_pin = args.seed_pin.as_deref().map(fs::read).transpose()?;
    let staged_report = stage_apps(&mut data, seed_pin.as_deref())?;

    let pins = BootstrapPins {
        build: &args.build,
        version: &args.firmware,
    };
    let bootstrap_report = stage_bootstrap_from_dir(&mut data, &args.bootstrap_package, &pins)?;

    let merge_report = if let Some(seed_dir) = &args.shader_seed {
        let seed_src = TreeSource::from_dir(seed_dir)?;
        Some(merge_tree(&mut data, &seed_src)?)
    } else {
        None
    };

    let data_spec = VolumeSpec {
        uuid: args.data_uuid,
        name: args.data_name,
        role: 0x40,
        features: 0x2,
        readonly_compatible_features: 0x0c,
        incompatible_features: 0x200,
        unicode: UnicodeVersion::IOS_27,
    };
    let volumes = [NewVolume {
        spec: &data_spec,
        tree: &data,
    }];
    let options = WriteOptions {
        target_blocks: args.target_bytes / BLOCK_SIZE as u64,
        areas: CheckpointAreas::Rule,
        time_ns: args.time_ns,
        formatted_by: "switch_apfs_w6".into(),
    };
    let source_pins = SourcePins {
        system_uuid: args.system_uuid,
    };
    let planned = plan(&src, &source_pins, &volumes, &options, &[])?;
    let mut target = FileTarget::create(&args.target, args.target_bytes)?;
    write(&src, &mut target, &planned)?;

    println!(
        "overlay: {} files {} dirs {} symlinks (skipped {} ._*); staged: {}; bootstrap: {:?}; merge: {:?}",
        overlay_report.files,
        overlay_report.dirs,
        overlay_report.symlinks,
        overlay_report.skipped_apple_double,
        staged_report.staged.len(),
        bootstrap_report,
        merge_report,
    );
    Ok(())
}

fn main() {
    let args = Args::parse();
    if let Err(err) = run(args) {
        eprintln!("error: {err}");
        std::process::exit(1);
    }
}
