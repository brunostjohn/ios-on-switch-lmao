// SPDX-License-Identifier: GPL-3.0-or-later
//! Writes catalog.json: the records catalog.bzl made, each with its release assets hashed. The result must be a
//! catalog this installer reads, whose every record it can run with every asset it takes.

use std::fs::File;
use std::io::{BufReader, ErrorKind, Read};
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use switch_catalog::{ASSET_ROLES, Catalog};

#[derive(Parser)]
struct Args {
    /// The catalog without assets, from catalog.bzl.
    #[arg(long)]
    records: PathBuf,
    /// One asset of a build: the build, the asset's role, its name in the release, and the file.
    #[arg(long, num_args = 4, value_names = ["BUILD", "ROLE", "NAME", "FILE"])]
    asset: Vec<String>,
    #[arg(long)]
    out: PathBuf,
}

fn hash(path: &PathBuf) -> Result<(u64, String), String> {
    let at = |e: std::io::Error| format!("{}: {e}", path.display());
    let mut file = BufReader::with_capacity(1 << 20, File::open(path).map_err(at)?);
    let (mut hasher, mut len, mut buf) = (Sha256::new(), 0u64, vec![0; 1 << 20]);
    loop {
        match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                hasher.update(&buf[..n]);
                len += n as u64;
            }
            Err(e) if e.kind() == ErrorKind::Interrupted => {}
            Err(e) => return Err(at(e)),
        }
    }
    Ok((len, hex::encode(hasher.finalize())))
}

fn add_assets(catalog: &mut Value, assets: &[String]) -> Result<(), String> {
    let builds = catalog["builds"].as_array_mut().ok_or("the records have no builds")?;
    for asset in assets.chunks(4) {
        let [build, role, name, file] = asset else {
            unreachable!("clap gives --asset four values")
        };
        let record = builds
            .iter_mut()
            .find(|record| record["build"] == build.as_str())
            .ok_or_else(|| format!("an asset for {build}, which the records don't hold"))?;
        let (size, sha256) = hash(&PathBuf::from(file))?;
        let entry = json!({"name": name, "size": size, "sha256": sha256});
        if record["assets"]
            .as_object_mut()
            .ok_or("a record without assets")?
            .insert(role.clone(), entry)
            .is_some()
        {
            return Err(format!("two {role} assets for {build}"));
        }
    }
    Ok(())
}

fn run(args: &Args) -> Result<(), String> {
    let records = std::fs::read(&args.records).map_err(|e| format!("{}: {e}", args.records.display()))?;
    let mut catalog: Value = serde_json::from_slice(&records).map_err(|e| e.to_string())?;
    add_assets(&mut catalog, &args.asset)?;
    let mut json = serde_json::to_vec_pretty(&catalog).map_err(|e| e.to_string())?;
    json.push(b'\n');

    let parsed = Catalog::parse(&json).map_err(|e| e.to_string())?;
    for record in &parsed.builds {
        let build = record.plan().map_err(|e| e.to_string())?;
        for role in ASSET_ROLES {
            build.asset(role).map_err(|e| e.to_string())?;
        }
    }
    std::fs::write(&args.out, json).map_err(|e| format!("{}: {e}", args.out.display()))
}

fn main() -> ExitCode {
    match run(&Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("switch_catalog_tool: {message}");
            ExitCode::FAILURE
        }
    }
}
