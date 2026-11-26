// SPDX-License-Identifier: GPL-3.0-or-later
//! `switch_apfs_read`: reads an APFS container image as the writer will, for the oracle tests and by hand.

mod capture;
mod listing;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use switch_apfs_format::io::FileSource;
use switch_apfs_read::{Container, Fs, Record, Step, Volume};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(clap::Args)]
struct Image {
    image: PathBuf,
    /// Where the container starts in the file, in bytes.
    #[arg(long, default_value_t = 0)]
    offset: u64,
}

#[derive(clap::Args)]
struct VolumeArg {
    /// The volume's UUID, name or index in the container.
    #[arg(long)]
    volume: String,
}

#[derive(Subcommand)]
enum Command {
    /// The container's volumes.
    Info(Image),
    /// One line per entry under a path, in the format `mount_list.py` prints for a mounted volume.
    Walk {
        #[command(flatten)]
        image: Image,
        #[command(flatten)]
        volume: VolumeArg,
        #[arg(long, default_value = "/")]
        path: String,
        /// Hash the content of one file in N, chosen by its path's hash; 0 hashes none.
        #[arg(long, default_value_t = 1)]
        hash_every: u32,
        /// Check every uncompressed stream against the seal's file-info hashes.
        #[arg(long)]
        check_file_info: bool,
    },
    /// Every block's owner against the space manager's bitmap. Fails on any problem.
    Audit {
        #[command(flatten)]
        image: Image,
        /// Print every claim: start, length, owner.
        #[arg(long)]
        list: bool,
    },
    /// Saves the fs-tree nodes a lookup of one object reads.
    Capture {
        #[command(flatten)]
        image: Image,
        #[command(flatten)]
        volume: VolumeArg,
        #[arg(long)]
        oid: u64,
        #[arg(long)]
        out: PathBuf,
    },
    /// Every fs-tree record of a volume, or of one object, as key and value in hex.
    Records {
        #[command(flatten)]
        image: Image,
        #[command(flatten)]
        volume: VolumeArg,
        #[arg(long)]
        oid: Option<u64>,
    },
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn main() -> ExitCode {
    match run(Cli::parse().command) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("switch_apfs_read: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(command: Command) -> Result<ExitCode> {
    match command {
        Command::Info(image) => info(&image),
        Command::Walk {
            image,
            volume,
            path,
            hash_every,
            check_file_info,
        } => walk(&image, &volume.volume, &path, hash_every, check_file_info),
        Command::Audit { image, list } => {
            let src = FileSource::open(&image.image, image.offset)?;
            let c = Container::open(&src)?;
            let audit = switch_apfs_read::audit(&c, &src)?;
            if list {
                for claim in &audit.claims {
                    println!("claim {} {} {:?}", claim.start, claim.len, claim.owner);
                }
            }
            print!("{audit}");
            Ok(if audit.ok() {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            })
        }
        Command::Capture {
            image,
            volume,
            oid,
            out,
        } => {
            let src = FileSource::open(&image.image, image.offset)?;
            let c = Container::open(&src)?;
            capture::capture(&Fs::open(&src, pick(&c, &volume.volume)?)?, oid, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Command::Records { image, volume, oid } => records(&image, &volume.volume, oid),
    }
}

fn pick<'c>(c: &'c Container, which: &str) -> Result<&'c Volume> {
    c.volumes
        .iter()
        .find(|v| {
            listing::uuid(&v.sb.vol_uuid).eq_ignore_ascii_case(which)
                || v.sb.name() == which
                || v.index.to_string() == which
        })
        .ok_or_else(|| format!("no volume {which}").into())
}

fn info(image: &Image) -> Result<ExitCode> {
    let src = FileSource::open(&image.image, image.offset)?;
    let c = Container::open(&src)?;
    println!(
        "container xid {} blocks {} free {} volumes {} chunk bitmaps {:?}",
        c.checkpoint.xid(),
        c.blocks(),
        c.space.free_count,
        c.volumes.len(),
        c.space.chunk_bitmaps
    );
    for v in &c.volumes {
        let fs = Fs::open(&src, v)?;
        let sb = &fs.sb;
        println!(
            "volume {} oid {} uuid {} role {:#x} incompat {:#x} sealed {} files {} dirs {} symlinks {} alloc {} name {}",
            v.index,
            v.oid,
            listing::uuid(&sb.vol_uuid),
            sb.role.get(),
            sb.incompatible_features.get(),
            fs.seal.is_some(),
            sb.num_files.get(),
            sb.num_directories.get(),
            sb.num_symlinks.get(),
            sb.fs_alloc_count.get(),
            sb.name()
        );
    }
    Ok(ExitCode::SUCCESS)
}

fn walk(image: &Image, volume: &str, path: &str, hash_every: u32, check: bool) -> Result<ExitCode> {
    let src = FileSource::open(&image.image, image.offset)?;
    let c = Container::open(&src)?;
    let fs = Fs::open(&src, pick(&c, volume)?)?;
    let mut checked = 0usize;
    let mut out = std::io::BufWriter::new(std::io::stdout().lock());
    let count = fs.walk(path, &mut |entry| {
        let line =
            listing::line(&src, entry, hash_every).map_err(|e| switch_apfs_read::ReadError::invalid(e.to_string()))?;
        use std::io::Write;
        writeln!(out, "{line}")?;
        if check {
            checked += listing::check_file_info(&src, entry)?;
        }
        Ok(Step::Into)
    })?;
    eprintln!("{count} entries; {checked} file-info hashes checked");
    Ok(ExitCode::SUCCESS)
}

fn records(image: &Image, volume: &str, oid: Option<u64>) -> Result<ExitCode> {
    let src = FileSource::open(&image.image, image.offset)?;
    let c = Container::open(&src)?;
    let fs = Fs::open(&src, pick(&c, volume)?)?;
    let hashed = fs.hashed_names();
    let mut print = |key: &[u8], value: &[u8]| -> std::result::Result<(), switch_apfs_read::ReadError> {
        let decoded = match Record::decode(key, value, hashed)? {
            Record::Other { oid, kind } => format!("other {oid} {kind}"),
            r => listing::kind_name(&r).to_string(),
        };
        println!("{decoded} {} {}", hex::encode(key), hex::encode(value));
        Ok(())
    };
    match oid {
        Some(oid) => {
            for r in fs.object(oid, None)? {
                print(&r.key, &r.value)?;
            }
        }
        None => {
            let visited = fs.records(&mut print)?;
            eprintln!("{} records in {} nodes", visited.records, visited.nodes.len());
        }
    }
    Ok(ExitCode::SUCCESS)
}
