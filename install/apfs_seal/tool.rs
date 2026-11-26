// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::Instant;

use clap::{Parser, Subcommand, ValueEnum};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::RESOURCE_FORK_XATTR_NAME;
use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_read::{Container, Fs, XattrData};
use switch_apfs_seal::delta::{load_manifest, parse_replace};
use switch_apfs_seal::{SealOptions, seal_fill, seal_to_file};
use switch_apfs_write::Fill;

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Seal {
        #[arg(long)]
        source: PathBuf,
        #[arg(long, default_value_t = 0)]
        source_offset: u64,
        #[arg(long)]
        out: PathBuf,
        #[arg(long, value_parser = parse_uuid)]
        uuid: [u8; 16],
        #[arg(long)]
        name: Option<String>,
        #[arg(long)]
        time_ns: u64,
        #[arg(long, default_value = "switch_apfs_seal")]
        formatted_by: String,
        #[arg(long)]
        manifest: Option<PathBuf>,
        /// `VOLUME_PATH=HOST_FILE[:SHA256_IN[:SHA256_OUT]]`
        #[arg(long = "replace")]
        replaces: Vec<String>,
        /// Write through `SealFill` (`switch_apfs_write::Fill`) instead of `seal_to_file`.
        #[arg(long)]
        via_fill: bool,
    },
    Inspect {
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
    },
    /// Flips one byte in a sealed image for negative-control testing.
    Flip {
        image: PathBuf,
        #[arg(long, value_enum)]
        target: FlipTarget,
        #[arg(long)]
        path: Option<String>,
    },
}

#[derive(Clone, Copy, ValueEnum)]
enum FlipTarget {
    Data,
    Leaf,
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn main() -> ExitCode {
    match run(Cli::parse().command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_apfs_seal: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(cmd: Command) -> Result<()> {
    match cmd {
        Command::Seal {
            source,
            source_offset,
            out,
            uuid,
            name,
            time_ns,
            formatted_by,
            manifest,
            replaces,
            via_fill,
        } => {
            let started = Instant::now();
            let mut entries = match manifest {
                Some(path) => load_manifest(&path)?,
                None => Vec::new(),
            };
            for spec in replaces {
                entries.push(parse_replace(&spec)?);
            }
            let options = SealOptions {
                volume_uuid: uuid,
                volume_name: name,
                time_ns,
                formatted_by,
            };
            let stats = if via_fill {
                let src = Arc::new(FileSource::open(&source, source_offset)?);
                let fill = seal_fill(src, &entries, &options)?;
                let stats = fill.plan().stats().clone();
                let mut target = FileTarget::create(&out, fill.exact_size())?;
                if let Err(err) = fill.fill(&mut target).and_then(|()| target.flush()) {
                    drop(target);
                    let _ = std::fs::remove_file(&out);
                    return Err(err.into());
                }
                stats
            } else {
                let src = FileSource::open(&source, source_offset)?;
                seal_to_file(&src, &entries, &options, &out)?
            };
            println!(
                "sealed {}: {} blocks ({} bytes, {} free), {} files ({} replaced), {} dirs, {} symlinks",
                stats.volume_name,
                stats.total_blocks,
                stats.total_bytes,
                stats.free_blocks,
                stats.files,
                stats.replaced_inodes,
                stats.dirs,
                stats.symlinks,
            );
            println!(
                "trees: fs {} nodes ({} levels), fext {}, extentref {}, omap {}; data {} blocks, metadata {} blocks",
                stats.fs_nodes,
                stats.fs_levels,
                stats.fext_nodes,
                stats.extentref_nodes,
                stats.omap_nodes,
                stats.data_blocks,
                stats.metadata_blocks,
            );
            println!("uuid: {}", uuid_string(&stats.volume_uuid));
            println!("root_hash: {}", hex::encode(stats.root_hash));
            println!("elapsed: {:.2?}", started.elapsed());
            Ok(())
        }
        Command::Inspect { image, offset } => {
            let src = FileSource::open(&image, offset)?;
            let c = Container::open(&src)?;
            println!(
                "container blocks {} free {} xid {} volumes {}",
                c.blocks(),
                c.space.free_count,
                c.checkpoint.xid(),
                c.volumes.len()
            );
            for v in &c.volumes {
                let fs = Fs::open(&src, v)?;
                let hash = fs.seal.map_or_else(|| "-".to_owned(), |s| hex::encode(s.root_hash));
                println!(
                    "volume {} uuid {} name {} sealed {} incompat {:#x} root_hash {}",
                    v.index,
                    uuid_string(&fs.sb.vol_uuid),
                    fs.sb.name(),
                    fs.seal.is_some(),
                    fs.sb.incompatible_features.get(),
                    hash
                );
            }
            Ok(())
        }
        Command::Flip { image, target, path } => flip(&image, target, path.as_deref()),
    }
}

fn flip(image: &Path, target: FlipTarget, path: Option<&str>) -> Result<()> {
    let src = FileSource::open(image, 0)?;
    let c = Container::open(&src)?;
    let vol = c.volumes.first().ok_or("no volume in image")?;
    let fs = Fs::open(&src, vol)?;
    let (paddr, offset_in_block) = match target {
        FlipTarget::Data => {
            let path = path.ok_or("--path is required for --target data")?;
            let oid = fs.lookup(path)?.ok_or_else(|| format!("{path} not found"))?;
            let inode = fs.inode(oid)?.ok_or_else(|| format!("inode {oid} missing"))?;
            let stream_id = if inode.is_compressed() {
                fs.xattrs(oid)?
                    .into_iter()
                    .find_map(|x| match x.data {
                        XattrData::Stream { id, .. } if x.name == RESOURCE_FORK_XATTR_NAME => Some(id),
                        _ => None,
                    })
                    .ok_or_else(|| format!("{path}: compressed without resource fork"))?
            } else {
                inode.val.private_id.get()
            };
            let extents = fs.extents(stream_id)?;
            let first = extents.first().ok_or_else(|| format!("{path} has no extents"))?;
            (first.phys_block_num, 0usize)
        }
        FlipTarget::Leaf => {
            let visited = fs.records(&mut |_, _| Ok(()))?;
            let leaf = *visited.nodes.last().ok_or("no fs-tree nodes")?;
            (leaf, 100usize)
        }
    };
    drop(fs);
    drop(src);
    let mut file = OpenOptions::new().read(true).write(true).open(image)?;
    let pos = paddr * BLOCK_SIZE as u64 + offset_in_block as u64;
    file.seek(SeekFrom::Start(pos))?;
    let mut byte = [0u8; 1];
    file.read_exact(&mut byte)?;
    byte[0] ^= 0xFF;
    file.seek(SeekFrom::Start(pos))?;
    file.write_all(&byte)?;
    file.sync_data()?;
    println!("flipped block {paddr} byte {offset_in_block}");
    Ok(())
}

fn parse_uuid(s: &str) -> std::result::Result<[u8; 16], String> {
    let hex: String = s.chars().filter(|c| *c != '-').collect();
    if hex.len() != 32 {
        return Err(format!("not a UUID: {s}"));
    }
    let mut out = [0u8; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|_| format!("not a UUID: {s}"))?;
    }
    Ok(out)
}

fn uuid_string(u: &[u8; 16]) -> String {
    let h = hex::encode_upper(u);
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}
