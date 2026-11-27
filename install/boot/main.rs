// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::BufReader;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

use clap::Parser;
use switch_boot::{Assets, LaunchdCatalog, Members, boot_files};
use switch_catalog::Catalog;

/// Writes a build's boot entry from its catalog record, the release's boot assets and the IPSW's files.
#[derive(Parser)]
struct Args {
    /// The release's catalog.json.
    #[arg(long)]
    catalog: PathBuf,
    #[arg(long)]
    build: String,
    /// The release's boot assets for the build (tar), checked against the catalog.
    #[arg(long)]
    assets: PathBuf,
    #[arg(long)]
    kernelcache: PathBuf,
    #[arg(long)]
    device_tree: PathBuf,
    #[arg(long)]
    static_trust_cache: PathBuf,
    #[arg(long)]
    system_cryptex_trust_cache: PathBuf,
    #[arg(long)]
    app_cryptex_trust_cache: PathBuf,
    #[arg(long)]
    installd: PathBuf,
    #[arg(long)]
    system_version: PathBuf,
    #[arg(long, requires = "launchd_catalog_signature")]
    launchd_catalog: Option<PathBuf>,
    #[arg(long, requires = "launchd_catalog")]
    launchd_catalog_signature: Option<PathBuf>,
    /// The directory the boot entry is written into.
    #[arg(long)]
    out: PathBuf,
}

fn read(path: &PathBuf) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
}

fn run(args: &Args) -> Result<(), String> {
    let start = Instant::now();
    let catalog = Catalog::parse(&read(&args.catalog)?).map_err(|e| e.to_string())?;
    let record = catalog
        .record(&args.build)
        .ok_or_else(|| format!("the catalog has no {}", args.build))?;
    let build = record.plan().map_err(|e| e.to_string())?;
    let asset = build.asset("boot").map_err(|e| e.to_string())?;
    let tar = File::open(&args.assets).map_err(|e| format!("{}: {e}", args.assets.display()))?;
    let assets = Assets::from_tar_checked(BufReader::new(tar), asset).map_err(|e| e.to_string())?;
    let catalog = match (&args.launchd_catalog, &args.launchd_catalog_signature) {
        (Some(plist), Some(signature)) => Some((read(plist)?, read(signature)?)),
        _ => None,
    };
    let [
        kernelcache,
        device_tree,
        static_trust,
        system_trust,
        app_trust,
        installd,
        system_version,
    ] = [
        &args.kernelcache,
        &args.device_tree,
        &args.static_trust_cache,
        &args.system_cryptex_trust_cache,
        &args.app_cryptex_trust_cache,
        &args.installd,
        &args.system_version,
    ]
    .map(read);
    let (kernelcache, device_tree, static_trust) = (kernelcache?, device_tree?, static_trust?);
    let (system_trust, app_trust, installd, system_version) = (system_trust?, app_trust?, installd?, system_version?);
    let members = Members {
        kernelcache: &kernelcache,
        device_tree: &device_tree,
        static_trust_cache: &static_trust,
        system_cryptex_trust_cache: &system_trust,
        app_cryptex_trust_cache: &app_trust,
        installd: &installd,
        system_version: &system_version,
        launchd_catalog: catalog
            .as_ref()
            .map(|(plist, signature)| LaunchdCatalog { plist, signature }),
    };
    let read_at = start.elapsed();
    let package = boot_files(&build, &assets, &members).map_err(|e| e.to_string())?;
    let made_at = start.elapsed();
    package
        .write(&args.out)
        .map_err(|e| format!("{}: {e}", args.out.display()))?;
    eprintln!(
        "{}: {} files, installd {}; read {:.2} s, made {:.2} s, written {:.2} s",
        build.build(),
        package.files.len(),
        hex::encode(package.installd_cdhash),
        read_at.as_secs_f64(),
        (made_at - read_at).as_secs_f64(),
        (start.elapsed() - made_at).as_secs_f64(),
    );
    Ok(())
}

fn main() -> ExitCode {
    match run(&Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
