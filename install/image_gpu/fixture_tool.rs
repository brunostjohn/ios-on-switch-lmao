// SPDX-License-Identifier: GPL-3.0-or-later
//! End-to-end driver applying `switch_image_gpu::stage_gpu_from_path` onto a seeded APFS Data `Tree` and committing
//! it into Apple's container via `switch_apfs_write`.

use std::path::PathBuf;

use clap::Parser;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::{CheckpointAreas, NewVolume, SourcePins, WriteOptions, plan, write};
use switch_image::{DEFAULT_DATA_NAME, DataVolumeParams, data_volume_spec, parse_uuid, seed_data};
use switch_image_gpu::stage_gpu_from_path;

#[derive(Parser)]
struct Args {
    #[arg(long)]
    source: PathBuf,
    #[arg(long)]
    target: PathBuf,
    #[arg(long)]
    target_bytes: u64,
    #[arg(long, value_parser = parse_uuid_arg)]
    system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid_arg)]
    data_uuid: [u8; 16],
    #[arg(long, default_value = DEFAULT_DATA_NAME)]
    data_name: String,
    #[arg(long)]
    gpu_stage: PathBuf,
    #[arg(long, default_value_t = 1_795_000_000_000_000_000)]
    time_ns: u64,
}

fn parse_uuid_arg(s: &str) -> Result<[u8; 16], String> {
    parse_uuid(s).map_err(|e| e.to_string())
}

fn run(args: Args) -> Result<(), Box<dyn std::error::Error>> {
    let src = FileSource::open(&args.source, 0)?;
    let container = Container::open(&src)?;
    let system = container
        .volume(args.system_uuid)
        .ok_or("source has no volume with the System UUID")?;
    let fs = Fs::open(&src, system)?;

    let mut data_tree = seed_data(&fs)?.tree;
    let report = stage_gpu_from_path(&mut data_tree, Some(&fs), &args.gpu_stage)?;

    let data_spec = data_volume_spec(&DataVolumeParams::new(
        args.data_uuid,
        args.data_name,
        UnicodeVersion::IOS_27,
    ));
    let volumes = [NewVolume {
        spec: &data_spec,
        tree: &data_tree,
    }];
    let options = WriteOptions {
        target_blocks: args.target_bytes / BLOCK_SIZE as u64,
        areas: CheckpointAreas::Rule,
        time_ns: args.time_ns,
        formatted_by: "switch_image_gpu".into(),
    };
    let source_pins = SourcePins {
        system_uuid: args.system_uuid,
    };
    let planned = plan(&src, &source_pins, &volumes, &options, &[])?;
    let mut target = FileTarget::create(&args.target, args.target_bytes)?;
    write(&src, &mut target, &planned)?;

    println!(
        "gpu staged: {} files, {} prefs, whole={}, icon_build={}",
        report.files, report.prefs, report.whole_digest, report.icon_build
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
