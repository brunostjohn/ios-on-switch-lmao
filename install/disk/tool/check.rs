// SPDX-License-Identifier: GPL-3.0-or-later
use std::error::Error;
use std::io::Write;
use std::path::PathBuf;
use std::process::Command;

use switch_disk::{OpenOptions, Privilege, RawDisk};

pub fn run(
    path: PathBuf,
    offset: u64,
    while_held: Option<String>,
    helper: bool,
    eject: bool,
) -> Result<(), Box<dyn Error>> {
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
    println!(
        "held {}: {} bytes, sectors {}/{}",
        path.display(),
        raw.size(),
        raw.logical_sector(),
        raw.physical_sector()
    );
    let still: Vec<_> = switch_disk::list()?
        .into_iter()
        .filter(|d| d.path == path)
        .flat_map(|d| d.mounted().map(|v| v.mount_point.clone()).collect::<Vec<_>>())
        .collect();
    expect(still.is_empty(), format!("nothing on it is mounted: {still:?}"))?;

    let mut aligned = raw.buffer(1 << 20);
    aligned.iter_mut().enumerate().for_each(|(i, b)| *b = (i % 253) as u8);
    raw.write_at(offset, &aligned)?;
    expect(
        read(&raw, offset, aligned.len())? == aligned[..],
        "aligned write reads back",
    )?;

    let small: Vec<u8> = (0..5001u32).map(|i| (i * 7) as u8).collect();
    raw.write_at(offset + 1000, &small[1..])?;
    let around = read(&raw, offset, 8192)?;
    expect(around[1000..6000] == small[1..], "unaligned write reads back")?;
    expect(
        around[..1000] == aligned[..1000] && around[6000..] == aligned[6000..8192],
        "its sectors keep their other bytes",
    )?;

    let start = offset + (2 << 20) + 3;
    let pieces: Vec<Vec<u8>> = [1usize, 7, 513, 4096, 100_000]
        .iter()
        .map(|&n| vec![n as u8; n])
        .collect();
    let mut writer = raw.writer(start)?;
    for piece in &pieces {
        writer.write_all(piece)?;
    }
    writer.finish()?;
    expect(
        read(&raw, start, pieces.concat().len())? == pieces.concat(),
        "gathered writes read back",
    )?;
    raw.flush()?;

    let mut refused = true;
    if let Some(command) = while_held {
        let status = shell(&command).status()?;
        refused = !status.success();
        println!(
            "while held, `{command}`: {status} ({})",
            if refused {
                "refused, as it must be"
            } else {
                "NOT REFUSED"
            }
        );
    }
    if eject {
        raw.eject()?;
        println!("ejected");
    } else {
        raw.release()?;
        println!("released");
    }
    expect(refused, "the command run while held failed")?;
    println!("check passed");
    Ok(())
}

fn read(raw: &RawDisk, offset: u64, len: usize) -> Result<Vec<u8>, Box<dyn Error>> {
    let mut buf = vec![0u8; len];
    raw.read_at(offset, &mut buf)?;
    Ok(buf)
}

fn expect(ok: bool, what: impl Into<String>) -> Result<(), Box<dyn Error>> {
    let what = what.into();
    if ok {
        println!("ok: {what}");
        Ok(())
    } else {
        Err(format!("FAILED: {what}").into())
    }
}

#[cfg(windows)]
fn shell(command: &str) -> Command {
    let mut c = Command::new("cmd");
    c.args(["/C", command]);
    c
}

#[cfg(not(windows))]
fn shell(command: &str) -> Command {
    let mut c = Command::new("/bin/sh");
    c.args(["-c", command]);
    c
}
