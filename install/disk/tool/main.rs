// SPDX-License-Identifier: GPL-3.0-or-later
mod check;

use std::error::Error;
use std::path::PathBuf;
use std::time::Instant;

use clap::{Parser, Subcommand};
use switch_disk::{OpenOptions, Privilege};

#[derive(Parser)]
struct Args {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Every disk, and why one can't be picked.
    List,
    /// Sequential write speed at each request size, flush included. Overwrites the disk from offset 0.
    Bench {
        disk: PathBuf,
        #[arg(long, default_value_t = 1024)]
        mib: usize,
        #[arg(long, value_delimiter = ',', default_value = "1,4,8")]
        requests: Vec<usize>,
        #[arg(long, default_value_t = 3)]
        repeat: usize,
        #[arg(long)]
        helper: bool,
    },
    /// Holds the disk, writes and reads back aligned and unaligned data at `--offset-mib`, runs `--while-held`
    /// (which must fail, as a mount of the held disk does), then releases or ejects it.
    Check {
        disk: PathBuf,
        #[arg(long)]
        offset_mib: u64,
        #[arg(long)]
        while_held: Option<String>,
        #[arg(long)]
        helper: bool,
        #[arg(long)]
        eject: bool,
    },
}

fn main() -> Result<(), Box<dyn Error>> {
    match Args::parse().command {
        Command::List => list(),
        Command::Bench {
            disk,
            mib,
            requests,
            repeat,
            helper,
        } => bench(disk, mib, &requests, repeat, helper),
        Command::Check {
            disk,
            offset_mib,
            while_held,
            helper,
            eject,
        } => check::run(disk, offset_mib << 20, while_held, helper, eject),
    }
}

fn list() -> Result<(), Box<dyn Error>> {
    for disk in switch_disk::list()? {
        let verdict = disk
            .refusal()
            .map_or("pickable".to_owned(), |r| format!("refused: {r}"));
        println!(
            "{} {} bytes, sectors {}/{}, {:?}, removable {}, internal {}, \"{}\" [{}] ({verdict})",
            disk.path.display(),
            disk.size,
            disk.logical_sector,
            disk.physical_sector,
            disk.bus,
            disk.removable,
            disk.internal,
            disk.model,
            disk.id
        );
        for volume in &disk.volumes {
            println!("  (whole disk) {} {}", volume.file_system, volume.mount_point.display());
        }
        for p in &disk.partitions {
            println!(
                "  {} #{} at {} size {} \"{}\" {}",
                p.path.display(),
                p.number,
                p.offset,
                p.size,
                p.name,
                p.kind
            );
            for volume in &p.volumes {
                println!("    {} {}", volume.file_system, volume.mount_point.display());
            }
        }
    }
    Ok(())
}

fn bench(path: PathBuf, mib: usize, requests: &[usize], repeat: usize, helper: bool) -> Result<(), Box<dyn Error>> {
    let disk = switch_disk::list()?
        .into_iter()
        .find(|d| d.path == path)
        .ok_or("no such disk")?;
    let privilege = if helper { Privilege::Helper } else { Privilege::Least };
    let raw = switch_disk::open(
        &disk,
        &OpenOptions {
            force_unmount: false,
            privilege,
        },
    )?;
    let total = (mib as u64) << 20;
    if total > raw.size() {
        return Err(format!("{} is smaller than {mib} MiB", path.display()).into());
    }
    for &request in requests {
        let len = request << 20;
        let mut buf = raw.buffer(len);
        buf.iter_mut().enumerate().for_each(|(i, b)| *b = (i % 251) as u8);
        let mut speeds = Vec::new();
        for _ in 0..repeat {
            let start = Instant::now();
            let mut offset = 0;
            while offset < total {
                raw.write_at(offset, &buf)?;
                offset += len as u64;
            }
            raw.flush()?;
            speeds.push(total as f64 / (1 << 20) as f64 / start.elapsed().as_secs_f64());
        }
        speeds.sort_by(f64::total_cmp);
        println!(
            "request {request} MiB: {mib} MiB x {repeat}, MiB/s min {:.0} median {:.0} max {:.0}",
            speeds[0],
            speeds[speeds.len() / 2],
            speeds[speeds.len() - 1]
        );
    }
    raw.release()?;
    Ok(())
}
