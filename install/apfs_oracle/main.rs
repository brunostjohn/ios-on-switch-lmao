// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand, ValueEnum};
use switch_apfs_names::UnicodeVersion;
use switch_apfs_oracle::compose::{ComposeConfig, compose};
use switch_apfs_oracle::parse::{
    SealedCryptexSpec, VerbatimCryptexSpec, parse_areas, parse_int, parse_sealed_cryptex, parse_uuid,
    parse_verbatim_cryptex,
};
use switch_apfs_oracle::{inspect, power_cuts, tamper, verify};
use switch_apfs_write::CheckpointAreas;
use switch_image::{DEFAULT_DATA_NAME, DEFAULT_PREBOOT_NAME};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Builds the complete 3-volume APFS container in one pass (with SealFill / FileFill).
    Compose {
        #[command(flatten)]
        args: PipelineArgs,
        #[arg(long)]
        target: PathBuf,
        #[arg(long)]
        expected_data_dump: Option<PathBuf>,
        #[arg(long)]
        expected_preboot_dump: Option<PathBuf>,
    },
    /// Asserts every Preboot cryptex is 1 contiguous uncompressed extent and optionally extracts it.
    VerifyPrebootExtents {
        #[arg(long)]
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
        #[arg(long, value_parser = parse_uuid)]
        preboot_uuid: Option<[u8; 16]>,
        #[arg(long)]
        extract_dir: Option<PathBuf>,
        #[arg(long)]
        expect_count: Option<usize>,
    },
    /// Decodes Switch/pretranslated/.index and .originals directly from the image and verifies all inode ids.
    VerifyIndex {
        #[arg(long)]
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
        #[arg(long, value_parser = parse_uuid)]
        system_uuid: Option<[u8; 16]>,
        #[arg(long, value_parser = parse_uuid)]
        data_uuid: Option<[u8; 16]>,
        #[arg(long)]
        expect_originals: bool,
        #[arg(long)]
        dump_tsv: Option<PathBuf>,
    },
    /// Verifies volume UUIDs, names, roles, feature bits, and spaceman geometry.
    VerifyPins {
        #[arg(long)]
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
        #[arg(long, value_parser = parse_uuid)]
        system_uuid: [u8; 16],
        #[arg(long, value_parser = parse_uuid)]
        data_uuid: [u8; 16],
        #[arg(long, default_value = DEFAULT_DATA_NAME)]
        data_name: String,
        #[arg(long, value_parser = parse_uuid)]
        preboot_uuid: [u8; 16],
        #[arg(long, default_value = DEFAULT_PREBOOT_NAME)]
        preboot_name: String,
        #[arg(long)]
        expected_blocks: Option<u64>,
    },
    /// Mutates an image copy in place for negative controls.
    Tamper {
        image: PathBuf,
        #[arg(value_enum)]
        kind: TamperKind,
        #[arg(long, value_parser = parse_uuid)]
        data_uuid: Option<[u8; 16]>,
        #[arg(long, value_parser = parse_uuid)]
        preboot_uuid: Option<[u8; 16]>,
        #[arg(long)]
        cryptex_name: Option<String>,
        #[arg(long)]
        cryptex_path: Option<String>,
        #[arg(long)]
        standalone: bool,
        #[arg(long, value_parser = parse_uuid)]
        new_uuid: Option<[u8; 16]>,
    },
    /// Writes phase-boundary cut images (B, C, D, E) and checks interruption/resume invariants.
    PowerCuts {
        #[command(flatten)]
        args: PipelineArgs,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Prints container and volume summary.
    Inspect {
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
    },
    /// Sets a volume's role in a fixture container.
    SetRole {
        image: PathBuf,
        #[arg(long, value_parser = parse_uuid)]
        uuid: [u8; 16],
        #[arg(long, value_parser = parse_int::<u16>)]
        role: u16,
    },
}

#[derive(Clone, Copy, ValueEnum)]
enum TamperKind {
    QueueLimit,
    DataLeaf,
    PrebootCryptexByte,
    WrongDataUuid,
}

#[derive(Args)]
struct PipelineArgs {
    #[arg(long)]
    source: PathBuf,
    #[arg(long, default_value_t = 0)]
    source_offset: u64,
    #[arg(long, value_parser = parse_int::<u64>)]
    target_bytes: u64,
    #[arg(long, value_parser = parse_uuid)]
    system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid)]
    data_uuid: [u8; 16],
    #[arg(long, default_value = DEFAULT_DATA_NAME)]
    data_name: String,
    #[arg(long, value_parser = parse_uuid)]
    preboot_uuid: [u8; 16],
    #[arg(long, default_value = DEFAULT_PREBOOT_NAME)]
    preboot_name: String,
    #[arg(long)]
    unicode: UnicodeVersion,
    #[arg(long)]
    time_ns: u64,
    #[arg(long, value_parser = parse_areas, default_value = "rule")]
    areas: CheckpointAreas,
    #[arg(long, default_value = "switch_apfs_oracle")]
    formatted_by: String,
    #[arg(long)]
    delta_dir: Option<PathBuf>,
    #[arg(long)]
    manifest_out: Option<PathBuf>,
    #[arg(long)]
    overlay_dir: Option<PathBuf>,
    #[arg(long)]
    builtin_pin: Option<PathBuf>,
    #[arg(long)]
    seed_pin: Option<PathBuf>,
    #[arg(long)]
    bootstrap_dir: Option<PathBuf>,
    #[arg(long)]
    bootstrap_helper: Option<PathBuf>,
    #[arg(long)]
    build: Option<String>,
    #[arg(long)]
    firmware: Option<String>,
    #[arg(long)]
    shader_seed_dir: Option<PathBuf>,
    #[arg(long = "sealed-cryptex", value_parser = parse_sealed_cryptex)]
    sealed_cryptexes: Vec<SealedCryptexSpec>,
    #[arg(long = "verbatim-cryptex", value_parser = parse_verbatim_cryptex)]
    verbatim_cryptexes: Vec<VerbatimCryptexSpec>,
}

impl PipelineArgs {
    fn into_config(self, expected_data_dump: Option<PathBuf>, expected_preboot_dump: Option<PathBuf>) -> ComposeConfig {
        ComposeConfig {
            source: self.source,
            source_offset: self.source_offset,
            target_bytes: self.target_bytes,
            system_uuid: self.system_uuid,
            data_uuid: self.data_uuid,
            data_name: self.data_name,
            preboot_uuid: self.preboot_uuid,
            preboot_name: self.preboot_name,
            unicode: self.unicode,
            time_ns: self.time_ns,
            areas: self.areas,
            formatted_by: self.formatted_by,
            delta_dir: self.delta_dir,
            manifest_out: self.manifest_out,
            overlay_dir: self.overlay_dir,
            builtin_pin: self.builtin_pin,
            seed_pin: self.seed_pin,
            bootstrap_dir: self.bootstrap_dir,
            bootstrap_helper: self.bootstrap_helper,
            build: self.build,
            firmware: self.firmware,
            shader_seed_dir: self.shader_seed_dir,
            sealed_cryptexes: self.sealed_cryptexes,
            verbatim_cryptexes: self.verbatim_cryptexes,
            expected_data_dump,
            expected_preboot_dump,
        }
    }
}

fn main() -> ExitCode {
    match run(Cli::parse().command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_apfs_oracle_tool: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cmd: Command) -> switch_apfs_oracle::Result<()> {
    match cmd {
        Command::Compose {
            args,
            target,
            expected_data_dump,
            expected_preboot_dump,
        } => {
            let cfg = args.into_config(expected_data_dump, expected_preboot_dump);
            compose(&cfg, &target)
        }
        Command::VerifyPrebootExtents {
            image,
            offset,
            preboot_uuid,
            extract_dir,
            expect_count,
        } => {
            let found =
                verify::verify_preboot_extents(&image, offset, preboot_uuid, extract_dir.as_deref(), expect_count)?;
            for item in found {
                println!(
                    "cryptex\t{}\tsize={}\tpaddr={}\tblocks={}\textents=1\tuncompressed=true\tsha256={}",
                    item.name,
                    item.size,
                    item.paddr,
                    item.blocks,
                    item.sha256.as_deref().unwrap_or("-"),
                );
            }
            Ok(())
        }
        Command::VerifyIndex {
            image,
            offset,
            system_uuid,
            data_uuid,
            expect_originals,
            dump_tsv,
        } => {
            let report = verify::verify_index(
                &image,
                offset,
                system_uuid,
                data_uuid,
                expect_originals,
                dump_tsv.as_deref(),
            )?;
            println!(
                "index: {} files, {} dirs, {} originals verified",
                report.files, report.dirs, report.originals
            );
            Ok(())
        }
        Command::VerifyPins {
            image,
            offset,
            system_uuid,
            data_uuid,
            data_name,
            preboot_uuid,
            preboot_name,
            expected_blocks,
        } => verify::verify_pins(
            &image,
            offset,
            &verify::PinExpectation {
                system_uuid,
                data_uuid,
                data_name: &data_name,
                preboot_uuid,
                preboot_name: &preboot_name,
                expected_blocks,
            },
        ),
        Command::Tamper {
            image,
            kind,
            data_uuid,
            preboot_uuid,
            cryptex_name,
            cryptex_path,
            standalone,
            new_uuid,
        } => match kind {
            TamperKind::QueueLimit => tamper::queue_limit(&image),
            TamperKind::DataLeaf => tamper::data_leaf(&image, data_uuid),
            TamperKind::PrebootCryptexByte => {
                let path = cryptex_path.as_deref().ok_or_else(|| {
                    switch_apfs_oracle::Error::Check("--cryptex-path is required for preboot-cryptex-byte".into())
                })?;
                tamper::preboot_cryptex_byte(&image, preboot_uuid, cryptex_name.as_deref(), path, standalone)
            }
            TamperKind::WrongDataUuid => tamper::wrong_data_uuid(&image, data_uuid, new_uuid),
        },
        Command::PowerCuts { args, out_dir } => {
            let cfg = args.into_config(None, None);
            power_cuts::run(&cfg, &out_dir)
        }
        Command::Inspect { image, offset } => inspect::inspect(&image, offset),
        Command::SetRole { image, uuid, role } => inspect::set_role(&image, uuid, role),
    }
}
