// SPDX-License-Identifier: GPL-3.0-or-later
//! An independent read of a container with dpp's `apfs` crate, which shares no code with ours: every volume's name,
//! UUID and root listing, and each named file's owner, mode and bytes.
//!
//!   switch_apfs_m1_check <image> [--offset N] [--file <volume name>:<root file>]...

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::process::ExitCode;

use apfs::{catalog, extents, object, omap, superblock};

struct Offset<R> {
    inner: R,
    base: u64,
}

impl<R: Read> Read for Offset<R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        self.inner.read(buf)
    }
}

impl<R: Seek> Seek for Offset<R> {
    fn seek(&mut self, pos: SeekFrom) -> std::io::Result<u64> {
        let pos = match pos {
            SeekFrom::Start(n) => SeekFrom::Start(n + self.base),
            other => other,
        };
        Ok(self.inner.seek(pos)? - self.base)
    }
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_apfs_m1_check: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let image = args
        .first()
        .ok_or("usage: switch_apfs_m1_check <image> [--offset N] [--file VOLUME:NAME]...")?;
    let mut base = 0u64;
    let mut files = Vec::new();
    let mut rest = args[1..].iter();
    while let Some(arg) = rest.next() {
        match arg.as_str() {
            "--offset" => base = rest.next().ok_or("--offset N")?.parse()?,
            "--file" => files.push(rest.next().ok_or("--file VOLUME:NAME")?.clone()),
            other => return Err(format!("unknown argument {other}").into()),
        }
    }
    let mut r = Offset {
        inner: File::open(image)?,
        base,
    };
    let nx = superblock::read_nxsb(&mut r)?;
    let nx = superblock::find_latest_nxsb(&mut r, &nx)?;
    let bs = nx.block_size;
    println!("container xid {} blocks {}", nx.header.xid, nx.block_count);
    let container_omap = omap::read_omap_tree_root(&mut r, nx.omap_oid, bs)?;
    for &oid in nx.fs_oids.iter().filter(|&&o| o != 0) {
        let paddr = omap::omap_lookup(&mut r, container_omap, bs, oid)?;
        let (_, block) = object::read_object(&mut r, paddr, bs)?;
        let sb = superblock::ApfsSuperblock::parse(&block)?;
        let uuid: String = sb.uuid.iter().map(|b| format!("{b:02X}")).collect();
        let volume_omap = omap::read_omap_tree_root(&mut r, sb.omap_oid, bs)?;
        let root = omap::omap_lookup(&mut r, volume_omap, bs, sb.root_tree_oid)?;
        let mut names: Vec<String> = catalog::list_directory(&mut r, root, volume_omap, bs, 2)?
            .into_iter()
            .map(|e| e.name)
            .collect();
        names.sort();
        println!(
            "volume {oid} {} {uuid} files {} dirs {}: {}",
            sb.volume_name,
            sb.num_files,
            sb.num_directories,
            names.join(" ")
        );
        for want in files
            .iter()
            .filter_map(|f| f.strip_prefix(&format!("{}:", sb.volume_name)))
        {
            let entry = catalog::list_directory(&mut r, root, volume_omap, bs, 2)?
                .into_iter()
                .find(|e| e.name == want)
                .ok_or_else(|| format!("{}: no /{want}", sb.volume_name))?;
            let inode = catalog::lookup_inode(&mut r, root, volume_omap, bs, entry.oid)?;
            let found = catalog::lookup_extents(&mut r, root, volume_omap, bs, inode.private_id)?;
            let mut bytes = Vec::new();
            extents::read_file_data(&mut r, bs, &found, inode.dstream_size.unwrap_or(0), &mut bytes)?;
            println!(
                "{}:/{want} {}:{} {:o} nlink {} {:?}",
                sb.volume_name,
                inode.uid,
                inode.gid,
                inode.mode,
                inode.nchildren_or_nlink,
                String::from_utf8_lossy(&bytes)
            );
        }
    }
    Ok(())
}
