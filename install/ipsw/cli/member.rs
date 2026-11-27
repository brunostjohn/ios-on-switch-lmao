// SPDX-License-Identifier: GPL-3.0-or-later
use std::error::Error;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;
use std::time::Duration;

use switch_catalog::Catalog;
use switch_fetch::{Span, Url, fetch_span};

const USAGE: &str = "switch_ipsw_member <catalog.json> <build> <role> <dir>";

/// One member of a build in the catalog: its stored bytes from the IPSW's URL into <dir> (resumable), then its content
/// beside them.
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [catalog, build, role, dir] = &args[..] else {
        return Err(USAGE.into());
    };
    let catalog = Catalog::parse(&std::fs::read(catalog)?)?;
    let record = catalog
        .record(build)
        .ok_or_else(|| format!("the catalog has no {build}"))?;
    let member = record
        .members
        .get(role)
        .ok_or_else(|| format!("{build} has no {role}"))?;
    let dir = Path::new(dir);
    let stored = dir.join(format!("{}.stored", member.file_name()));
    let client = reqwest::Client::builder()
        .user_agent("switch-installer")
        .read_timeout(Duration::from_secs(60))
        .build()?;
    let total = member.stored_size;
    let span = Span::member(&Url::parse(&record.ipsw.url)?, member);
    fetch_span(&client, &span, &stored, |done| eprint!("\r{done}/{total}")).await?;
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
