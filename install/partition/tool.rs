// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_partition_tool <card image> [--ios-gib N] [--image-used-bytes B] [--dry-run]
//!
//! Finishes a run a cut left on the image, then carves an iOS entry of N GiB (the picker's default when absent) out of
//! the image's FAT and prints how fast the files moved.

use std::fs::OpenOptions;
use std::process::ExitCode;

use switch_card::StreamDevice;

fn main() -> ExitCode {
    match run(std::env::args().skip(1).collect()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_partition_tool: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(args: Vec<String>) -> Result<(), Box<dyn std::error::Error>> {
    let usage = "usage: switch_partition_tool <card image> [--ios-gib N] [--image-used-bytes B] [--dry-run]";
    let mut image = None;
    let (mut ios_gib, mut used, mut dry) = (None, 0u64, false);
    let mut it = args.into_iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--ios-gib" => ios_gib = Some(it.next().ok_or(usage)?.parse::<u64>()?),
            "--image-used-bytes" => used = it.next().ok_or(usage)?.parse()?,
            "--dry-run" => dry = true,
            _ if image.is_none() && !a.starts_with("--") => image = Some(a),
            _ => return Err(usage.into()),
        }
    }
    let image = image.ok_or(usage)?;
    let file = OpenOptions::new().read(true).write(!dry).open(&image)?;
    let mut dev = StreamDevice::file(file, 512)?;
    if dry {
        if let Some(plan) = switch_partition::pending(&mut dev)? {
            println!("a cut-short run is pending: {plan:?}");
            return Ok(());
        }
    } else if let Some(report) = switch_partition::resume(&mut dev)? {
        println!("resumed a cut-short run: {report:?}");
        return Ok(());
    }
    let survey = switch_partition::survey(&mut dev)?;
    let limits = survey.limits(used);
    println!(
        "boot volume at {} for {} sectors{}; iOS can have {} to {} GiB",
        survey.layout.fat_first,
        survey.layout.fat_sectors,
        if survey.mbr_only() { ", MBR only" } else { "" },
        limits.min >> 30,
        limits.max >> 30
    );
    let bytes = ios_gib.map_or(limits.default, |g| g << 30);
    let plan = survey.plan(bytes, limits.min)?;
    println!(
        "entry {} at {} for {} sectors; the FAT keeps {} sectors",
        plan.entry_index, plan.entry_first, plan.entry_sectors, plan.fat_new_sectors
    );
    if dry {
        return Ok(());
    }
    let report = switch_partition::run(&mut dev, &plan)?;
    println!(
        "moved {} MiB in {} batches at {:.1} MiB/s",
        report.moved_bytes >> 20,
        report.batches,
        report.throughput / f64::from(1 << 20)
    );
    Ok(())
}
