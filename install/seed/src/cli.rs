// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::io::{BufReader, Read};
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

use clap::Parser;
use sha2::{Digest, Sha256};
use switch_seed::{SeedInputs, SeedOptions, build_seed, read_bundle_stack_key};
use switch_seed_plan::Depth;

#[derive(Debug, Parser)]
#[command(
    name = "switch_seed_cli",
    about = "Build Switch GPU pipeline pack and native shader seed"
)]
struct Cli {
    #[arg(long)]
    build: String,
    #[arg(long)]
    qc_sha256: String,
    #[arg(long)]
    seed_tar: PathBuf,
    #[arg(long)]
    os: PathBuf,
    #[arg(long)]
    system_cryptex: PathBuf,
    #[arg(long)]
    app_cryptex: PathBuf,
    #[arg(long)]
    work_dir: PathBuf,
    #[arg(long, default_value = "deep")]
    depth: String,
    #[arg(long, default_value_t = 256, value_parser = clap::value_parser!(u32).range(1..=4096))]
    native_buckets: u32,
    #[arg(long)]
    threads: Option<usize>,
    #[arg(long)]
    skip_native: bool,
    #[arg(long)]
    bundle_plist: Option<PathBuf>,
}

fn sha256_path(path: &PathBuf) -> Result<String, Box<dyn std::error::Error>> {
    let mut reader = BufReader::new(File::open(path)?);
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hex::encode(hasher.finalize()))
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    let depth = Depth::parse(&cli.depth)?;
    let seed_tar_sha256 = sha256_path(&cli.seed_tar)?;
    let bundle_stack = match &cli.bundle_plist {
        Some(p) => read_bundle_stack_key(&fs::read(p)?)?,
        None => None,
    };
    let inputs = SeedInputs {
        build: &cli.build,
        qc_sha256: &cli.qc_sha256,
        seed_tar: &cli.seed_tar,
        seed_tar_sha256: &seed_tar_sha256,
        os_container: &cli.os,
        system_cryptex: &cli.system_cryptex,
        app_cryptex: &cli.app_cryptex,
        os_sha256: None,
        system_cryptex_sha256: None,
        app_cryptex_sha256: None,
        bundle_stack_key: bundle_stack.as_deref(),
        work_dir: &cli.work_dir,
    };
    let opts = SeedOptions {
        depth,
        native_buckets: cli.native_buckets as usize,
        threads: cli.threads,
        skip_native: cli.skip_native,
        ..SeedOptions::default()
    };
    let cancel = AtomicBool::new(false);
    let out = build_seed(&inputs, &opts, &cancel, |stage, done, total, detail| {
        eprintln!("[{stage}] {done}/{total}: {detail}");
    })?;
    println!(
        "stack={} files={} pack={}",
        out.stack_key,
        out.staged_files.len(),
        out.pack_bin.display()
    );
    Ok(())
}
