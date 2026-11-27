// SPDX-License-Identifier: GPL-3.0-or-later
//! The byte offset, in the member, of a byte of one Mach-O's stored data (its resource fork when compressed),
//! for refusal_test's corrupted copy.

use std::path::PathBuf;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::FileSource;
use switch_translate::volume::{Data, Volume};

#[derive(clap::Args)]
pub struct Args {
    /// A cryptex: a container of one volume.
    #[arg(long)]
    image: PathBuf,
}

pub fn run(args: &Args) -> super::Result<()> {
    let src = FileSource::open(&args.image, 0)?;
    let volume = Volume::open(&src, None)?;
    for f in volume.files(&[])? {
        let stream = match &f.data {
            Data::Plain(s) | Data::Compressed { fork: Some(s), .. } => s,
            _ => continue,
        };
        let Some(e) = stream.extents.iter().find(|e| e.phys_block_num != 0) else {
            continue;
        };
        if f.macho_contents(&src, volume.sealed)?.is_some() {
            println!("{} {}", e.phys_block_num * BLOCK_SIZE as u64 + 100, f.path);
            return Ok(());
        }
    }
    Err("no Mach-O with stored data".into())
}
