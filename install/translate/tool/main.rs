// SPDX-License-Identifier: GPL-3.0-or-later
//! `switch_translate_tool`: one role's pretranslate delta from its decoded member, as //install:delta_<role> makes it
//! with translate_tree.py; and decmpfs against a mounted volume, for the remote checks.

mod decmpfs_check;
mod locate;
mod usage;

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};
use switch_apfs_format::io::FileSource;
use switch_translate::{CdhashPair, Inputs, Pins, Role, edits};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// One role's delta into OUT, which must not exist.
    Role(RoleArgs),
    /// Every compressed file of a volume, decompressed here, against the kernel's read of it through a mount.
    DecmpfsCheck(decmpfs_check::Args),
    /// Where a byte of one of a cryptex's Mach-O files is stored, for a corrupted copy.
    #[command(hide = true)]
    Locate(locate::Args),
}

#[derive(clap::Args)]
struct RoleArgs {
    #[arg(long)]
    role: String,
    /// The decoded member: the OS image or a cryptex.
    #[arg(long)]
    image: PathBuf,
    /// The IPSW's pins (//install:ipsw_pins), for the OS image's System volume.
    #[arg(long)]
    ipsw_pins: Option<PathBuf>,
    /// Apple's trust cache IM4Ps, in the order translate_tree takes them.
    #[arg(long)]
    apple_trustcache: Vec<PathBuf>,
    #[arg(long)]
    edits: Option<PathBuf>,
    /// Another role's <role>.cdhash-pairs, for the SystemOS cryptex's prebuilt loaders.
    #[arg(long)]
    cdhash_pairs: Vec<PathBuf>,
    /// The translator version the manifest records.
    #[arg(long, conflicts_with = "translator_of")]
    translator: Option<String>,
    /// The `arm64e` CLI whose version (its SHA-256's first 16 hex digits) the manifest records.
    #[arg(long)]
    translator_of: Option<PathBuf>,
    /// The release's pins of the run: {file name: sha256}.
    #[arg(long)]
    pins: PathBuf,
    #[arg(long)]
    out: PathBuf,
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Role(args) => role(&args),
        Command::DecmpfsCheck(args) => decmpfs_check::run(&args),
        Command::Locate(args) => locate::run(&args),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_translate_tool: {e}");
            ExitCode::FAILURE
        }
    }
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn role(args: &RoleArgs) -> Result<()> {
    let start = Instant::now();
    let role = Role::parse(&args.role).ok_or_else(|| format!("no role {:?}", args.role))?;
    let volume = match (role, &args.ipsw_pins) {
        (Role::Os, Some(pins)) => Some(os_volume(pins)?),
        (Role::Os, None) => return Err("the os role needs --ipsw-pins".into()),
        _ => None,
    };
    let apple = args
        .apple_trustcache
        .iter()
        .map(|p| im4p_payload(&fs::read(p)?))
        .collect::<Result<Vec<_>>>()?;
    let edits = match &args.edits {
        Some(p) => edits::parse(&fs::read(p)?)?,
        None => BTreeMap::new(),
    };
    let mut pairs: Vec<CdhashPair> = Vec::new();
    for p in &args.cdhash_pairs {
        pairs.extend(switch_translate::parse_pairs(&fs::read_to_string(p)?)?);
    }
    let translator = match (&args.translator, &args.translator_of) {
        (Some(v), _) => v.clone(),
        (None, Some(cli)) => hex::encode(Sha256::digest(fs::read(cli)?))[..16].to_owned(),
        (None, None) => return Err("give --translator or --translator-of".into()),
    };
    let pins = Pins::parse(&fs::read(&args.pins)?)?;
    let src = FileSource::open(&args.image, 0)?;
    let inputs = Inputs {
        role,
        source: &src,
        volume,
        apple_trust: &apple,
        edits: &edits,
        pairs: &pairs,
        translator: &translator,
        pins: &pins,
    };
    let delta = switch_translate::translate_file(&inputs, &args.image, 0, &args.out)?;
    for line in &delta.log {
        println!("{line}");
    }
    println!(
        "{}: {} files in {} ({:.1?} wall, peak RSS {})",
        role.name(),
        delta.files,
        delta.dir.display(),
        start.elapsed(),
        usage::peak_rss()
    );
    Ok(())
}

fn os_volume(pins: &PathBuf) -> Result<[u8; 16]> {
    let json: serde_json::Value = serde_json::from_slice(&fs::read(pins)?)?;
    let text = json["volumes"]["os_volume_uuid"]
        .as_str()
        .ok_or("the IPSW's pins name no os_volume_uuid")?;
    let bytes = hex::decode(text.replace('-', ""))?;
    Ok(bytes.try_into().map_err(|_| format!("{text}: not a UUID"))?)
}

fn im4p_payload(im4p: &[u8]) -> Result<Vec<u8>> {
    let tag = image4::PayloadRef::parse(im4p)
        .map_err(|e| format!("not an IM4P: {e}"))?
        .tag()
        .to_bytes();
    Ok(switch_img4::payload(im4p, tag, None)?)
}
