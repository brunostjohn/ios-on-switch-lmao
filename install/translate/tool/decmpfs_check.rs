// SPDX-License-Identifier: GPL-3.0-or-later
//! Every compressed file of a volume, decompressed by switch_decmpfs (and, on a sealed volume, checked against the
//! seal), against macOS's own read of it through a read-only mount of the same image.

use std::collections::BTreeMap;
use std::fs::File as HostFile;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use rayon::prelude::*;
use sha2::{Digest, Sha256};
use switch_apfs_format::io::FileSource;
use switch_translate::volume::{Data, File, Volume};

#[derive(clap::Args)]
pub struct Args {
    #[arg(long)]
    image: PathBuf,
    /// The volume's UUID; a container of one volume needs none.
    #[arg(long)]
    volume: Option<String>,
    /// Where macOS mounted the same volume.
    #[arg(long)]
    mount: PathBuf,
}

#[derive(Default)]
struct Tally {
    files: u64,
    bytes: u64,
    matched: u64,
}

pub fn run(args: &Args) -> super::Result<()> {
    let src = FileSource::open(&args.image, 0)?;
    let uuid = match &args.volume {
        Some(text) => Some(
            hex::decode(text.replace('-', ""))?
                .try_into()
                .map_err(|_| format!("{text}: not a UUID"))?,
        ),
        None => None,
    };
    let volume = Volume::open(&src, uuid)?;
    let sealed = volume.sealed;
    let compressed: Vec<File> = volume
        .files(&[])?
        .into_iter()
        .filter(|f| matches!(f.data, Data::Compressed { .. }))
        .collect();
    let tallies: Mutex<BTreeMap<u32, Tally>> = Mutex::default();
    let failures: Mutex<Vec<String>> = Mutex::default();
    compressed.par_iter().for_each(|f| {
        let outcome = (|| -> super::Result<(u32, u64, bool)> {
            let kind = f.compression(&src)?.map_or(0, |h| h.kind);
            let ours = f.contents(&src, sealed)?;
            let theirs = kernel_sha256(&args.mount, &f.path)?;
            Ok((kind, ours.len() as u64, Sha256::digest(&ours[..])[..] == theirs[..]))
        })();
        match outcome {
            Ok((kind, len, same)) => {
                let mut t = tallies.lock().expect("no panics hold it");
                let t = t.entry(kind).or_default();
                t.files += 1;
                t.bytes += len;
                t.matched += u64::from(same);
                if !same {
                    failures
                        .lock()
                        .expect("no panics hold it")
                        .push(format!("{}: differs", f.path));
                }
            }
            Err(e) => failures
                .lock()
                .expect("no panics hold it")
                .push(format!("{}: {e}", f.path)),
        }
    });
    for (kind, t) in tallies.into_inner().unwrap_or_else(|p| p.into_inner()) {
        println!(
            "decmpfs type {kind}: {} files, {} bytes, {} equal to the kernel's read",
            t.files, t.bytes, t.matched
        );
    }
    let failures = failures.into_inner().unwrap_or_else(|p| p.into_inner());
    for f in failures.iter().take(20) {
        println!("FAIL {f}");
    }
    if failures.is_empty() {
        println!("{} compressed files, all equal", compressed.len());
        Ok(())
    } else {
        Err(format!("{} of {} compressed files failed", failures.len(), compressed.len()).into())
    }
}

fn kernel_sha256(mount: &Path, path: &str) -> super::Result<Vec<u8>> {
    let mut file = HostFile::open(mount.join(path.trim_start_matches('/')))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            return Ok(h.finalize().to_vec());
        }
        h.update(&buf[..n]);
    }
}
