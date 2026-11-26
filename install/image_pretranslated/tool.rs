// SPDX-License-Identifier: GPL-3.0-or-later
//! Fixture driver exercising `switch_image`, `switch_image_pretranslated`, and `switch_apfs_write` end to end.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{FileSource, FileTarget};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::{CheckpointAreas, NewVolume, SourcePins, WriteOptions, plan, write};
use switch_image::{
    CryptexContent, CryptexImage, DEFAULT_DATA_NAME, DEFAULT_PREBOOT_NAME, DataVolumeParams, DeltaDir,
    PrebootVolumeParams, apply_var_delta, build_preboot, data_volume_spec, load_manifest, parse_uuid,
    preboot_volume_spec, seed_data, tree_entries,
};
use switch_image_pretranslated::{overlay_index, parse_cdhash_pairs, place_pretranslated};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Compose(ComposeArgs),
    DecodeIndex {
        index: PathBuf,
        #[arg(long)]
        originals: Option<PathBuf>,
    },
}

#[derive(clap::Args)]
struct ComposeArgs {
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
    #[arg(long, value_parser = parse_uuid_arg)]
    preboot_uuid: [u8; 16],
    #[arg(long)]
    unicode: UnicodeVersion,
    #[arg(long)]
    time_ns: u64,
    #[arg(long)]
    delta_dir: Option<PathBuf>,
    #[arg(long = "cryptex", value_parser = parse_cryptex)]
    cryptexes: Vec<CryptexArg>,
    #[arg(long)]
    manifest_out: Option<PathBuf>,
}

#[derive(Clone)]
struct CryptexArg {
    name: String,
    path: PathBuf,
    sha256: Option<[u8; 32]>,
}

fn parse_uuid_arg(s: &str) -> Result<[u8; 16], String> {
    parse_uuid(s).map_err(|e| e.to_string())
}

fn parse_cryptex(s: &str) -> Result<CryptexArg, String> {
    let mut parts = s.splitn(3, ':');
    let (Some(name), Some(path), sha) = (parts.next(), parts.next(), parts.next()) else {
        return Err(format!("expected NAME:PATH[:SHA256], got {s:?}"));
    };
    let sha256 = match sha {
        Some("") | None => None,
        Some(h) => {
            let mut out = [0u8; 32];
            hex::decode_to_slice(h, &mut out).map_err(|_| format!("invalid sha256 {h:?}"))?;
            Some(out)
        }
    };
    Ok(CryptexArg {
        name: name.into(),
        path: PathBuf::from(path),
        sha256,
    })
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_image_fixture_tool: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> Result<(), Box<dyn std::error::Error>> {
    match cli.command {
        Command::Compose(args) => compose(&args),
        Command::DecodeIndex { index, originals } => decode_index(&index, originals.as_deref()),
    }
}

fn compose(args: &ComposeArgs) -> Result<(), Box<dyn std::error::Error>> {
    let src = FileSource::open(&args.source, 0)?;
    let container = Container::open(&src)?;
    let system_vol = container
        .volume(args.system_uuid)
        .ok_or("source has no volume with the System UUID")?;
    let system_fs = Fs::open(&src, system_vol)?;

    let mut data = seed_data(&system_fs)?.tree;
    if let Some(delta_dir) = &args.delta_dir {
        let entries = load_manifest(&delta_dir.join("os.manifest.json"))?;
        let loader = DeltaDir(delta_dir.clone());
        let var_stats = apply_var_delta(&mut data, &system_fs, &entries, &loader)?;
        let pairs_path = delta_dir.join("os.cdhash-pairs");
        let pairs = if pairs_path.is_file() {
            parse_cdhash_pairs(&fs::read_to_string(&pairs_path)?)?
        } else {
            Vec::new()
        };
        let placed = place_pretranslated(&mut data, &system_fs, &tree_entries(&entries), &loader, &pairs)?;
        if let Some(out_path) = &args.manifest_out {
            fs::write(out_path, &placed.manifest_json)?;
        }
        println!(
            "delta: var replaced={} hard_links={}, pretranslated placed={} hard_links={} dirs={}",
            var_stats.replaced, var_stats.hard_links, placed.placed_files, placed.hard_links, placed.directories
        );
    }

    let images: Vec<CryptexImage> = args
        .cryptexes
        .iter()
        .map(|c| CryptexImage {
            name: c.name.clone(),
            content: CryptexContent::File {
                path: c.path.clone(),
                expected_sha256: c.sha256,
            },
        })
        .collect();
    let preboot = build_preboot(&images)?;

    let data_spec = data_volume_spec(&DataVolumeParams::new(args.data_uuid, DEFAULT_DATA_NAME, args.unicode));
    let preboot_spec = preboot_volume_spec(&PrebootVolumeParams::new(
        args.preboot_uuid,
        DEFAULT_PREBOOT_NAME,
        args.unicode,
    ));
    let options = WriteOptions {
        target_blocks: args.target_bytes / BLOCK_SIZE as u64,
        areas: CheckpointAreas::Rule,
        time_ns: args.time_ns,
        formatted_by: "switch_image".into(),
    };
    let pins = SourcePins {
        system_uuid: args.system_uuid,
    };
    let planned = plan(
        &src,
        &pins,
        &[
            NewVolume {
                spec: &data_spec,
                tree: &data,
            },
            NewVolume {
                spec: &preboot_spec,
                tree: &preboot,
            },
        ],
        &options,
        &[],
    )?;
    let mut target = FileTarget::create(&args.target, args.target_bytes)?;
    write(&src, &mut target, &planned)?;
    Ok(())
}

fn decode_index(index_path: &Path, originals_path: Option<&Path>) -> Result<(), Box<dyn std::error::Error>> {
    let raw = fs::read(index_path)?;
    let (files, dirs) = overlay_index::decode(&raw)?;
    for f in files {
        println!(
            "file\t{}\t{}\t{}\t{}",
            f.path,
            f.system_id,
            f.data_id,
            hex::encode(f.cdhash)
        );
    }
    for d in dirs {
        println!("dir\t{d}");
    }
    if let Some(orig_path) = originals_path {
        let orig_raw = fs::read(orig_path)?;
        for p in overlay_index::decode_originals(&orig_raw)? {
            println!("orig\t{}\t{}", hex::encode(p.original), hex::encode(p.translated));
        }
    }
    Ok(())
}
