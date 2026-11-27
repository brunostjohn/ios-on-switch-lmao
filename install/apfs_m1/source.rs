// SPDX-License-Identifier: GPL-3.0-or-later
//! `source`: an IPSW's OS member as the bare APFS container the writer reads, through the installer's own crates:
//! `switch_aea` decrypts 24B119's archive, `switch_ipsw` unwraps 21H461's UDIF.

use std::fs::File;
use std::io::{BufReader, BufWriter, Write};
use std::path::PathBuf;

use switch_ipsw::Container;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(clap::Args)]
pub struct SourceArgs {
    /// The member as the IPSW stores it.
    #[arg(long)]
    member: PathBuf,
    /// The member's pinned `aea_key`, when it is an encrypted archive.
    #[arg(long)]
    aea_key: Option<String>,
    /// The pinned `content_sha256` of the decrypted member.
    #[arg(long)]
    content_sha256: Option<String>,
    #[arg(long)]
    out: PathBuf,
}

pub fn run(a: &SourceArgs) -> Result<()> {
    let started = std::time::Instant::now();
    let mut out = BufWriter::with_capacity(8 << 20, File::create_new(&a.out)?);
    let bytes = match &a.aea_key {
        Some(key) => {
            let key = switch_aea::key_value(key)?;
            let sha: [u8; 32] = hex::decode(a.content_sha256.as_deref().ok_or("--content-sha256 is required")?)?
                .try_into()
                .map_err(|_| "a SHA-256 is 32 bytes")?;
            switch_aea::decrypt(File::open(&a.member)?, &key, &mut out, &sha, |_| {})?
        }
        None => switch_ipsw::os_container(BufReader::new(File::open(&a.member)?), &mut out, Container::Udif)?.len,
    };
    out.flush()?;
    println!("{}: {bytes} bytes in {:.1?}", a.out.display(), started.elapsed());
    Ok(())
}
