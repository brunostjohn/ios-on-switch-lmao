// SPDX-License-Identifier: GPL-3.0-or-later
use std::error::Error;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;
use std::time::Duration;

use switch_fetch::{Url, fetch_span};

const USAGE: &str = "switch_ipsw_member <pins.json> <kind> <role> <dir>";

/// One pinned member: its stored bytes from the IPSW's URL into <dir> (resumable), then its content beside them.
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [pins, kind, role, dir] = &args[..] else {
        return Err(USAGE.into());
    };
    let pins = switch_ipsw::parse_pins(&std::fs::read_to_string(pins)?)?;
    let ipsw = pins.get(kind).ok_or_else(|| format!("no kind {kind}"))?;
    let member = ipsw.members.get(role).ok_or_else(|| format!("{kind} has no {role}"))?;
    let dir = Path::new(dir);
    let stored = dir.join(format!("{}.stored", member.file_name()));
    let client = reqwest::Client::builder()
        .user_agent("switch-installer")
        .read_timeout(Duration::from_secs(60))
        .build()?;
    let total = member.stored_size;
    fetch_span(&client, &member.span(&Url::parse(&ipsw.url)?), &stored, |done| {
        eprint!("\r{done}/{total}")
    })
    .await?;
    eprintln!();
    let content = dir.join(member.file_name());
    let mut out = BufWriter::with_capacity(4 << 20, File::create(&content)?);
    let written = switch_ipsw::extract(File::open(&stored)?, member, &mut out)?;
    out.flush()?;
    println!(
        "{}: {} bytes, sha256 {}",
        content.display(),
        written.len,
        hex::encode(written.sha256)
    );
    Ok(())
}
