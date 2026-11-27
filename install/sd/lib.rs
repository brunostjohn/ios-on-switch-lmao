// SPDX-License-Identifier: GPL-3.0-or-later
//! The boot files on the card's FAT boot volume (FAT32 and exFAT, both through `hadris-fat`), on an allowlist:
//! `switchroot/ios/*`, `bootloader/ini/ios.ini` and `bootloader/res/icon_ios_hue.bmp`. One boot entry, replaced in
//! place: a file whose bytes are already there is left alone, and a file in `switchroot/ios/` the package no longer
//! has is removed, unless it is the user's (`allow::user_owned`). Nothing outside the allowlist is read for writing,
//! written or removed.

mod allow;
mod exfat;
mod fat32;
mod stream;

use std::io::{self, Read};

use switch_card::Device;

pub use allow::{BOOT_ARGS, BOOT_DIR, ICON, INI, user_owned};
pub use stream::VolumeStream;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("card I/O: {0}")]
    Io(#[from] io::Error),
    #[error("{path}: {why}")]
    Refused { path: String, why: &'static str },
    #[error("the boot volume is {0}")]
    Unsupported(&'static str),
    #[error("FAT32: {0}")]
    Fat(String),
    #[error("exFAT: {0}")]
    Exfat(String),
}

pub struct BootFile<'a> {
    /// Relative to the volume's root, `/`-separated.
    pub path: &'a str,
    pub data: &'a [u8],
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Summary {
    pub written: Vec<String>,
    pub unchanged: Vec<String>,
    pub removed: Vec<String>,
}

pub(crate) struct Item {
    pub name: String,
    pub dir: bool,
}

/// What the two file systems give the replace.
pub(crate) trait Tree {
    fn read(&mut self, path: &str) -> Result<Option<Vec<u8>>, Error>;
    /// Creates the parent directories as needed and replaces the file's contents.
    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), Error>;
    /// The directory's entries, none when it does not exist.
    fn list(&mut self, dir: &str) -> Result<Vec<Item>, Error>;
    fn remove(&mut self, path: &str) -> Result<(), Error>;
    fn close(self: Box<Self>) -> Result<(), Error>;
}

/// Makes the boot entry on the volume at `first` (`sectors` long) the package `files`.
pub fn install<D: Device>(dev: &mut D, first: u64, sectors: u64, files: &[BootFile]) -> Result<Summary, Error> {
    for (i, f) in files.iter().enumerate() {
        allow::check(f.path)?;
        if files[..i].iter().any(|g| g.path.eq_ignore_ascii_case(f.path)) {
            return Err(Error::Refused {
                path: f.path.to_string(),
                why: "listed twice",
            });
        }
    }
    let mut tree = open(&mut *dev, first, sectors)?;
    let summary = replace(&mut *tree, files)?;
    tree.close()?;
    dev.barrier()?;
    Ok(summary)
}

fn open<'d, D: Device>(dev: &'d mut D, first: u64, sectors: u64) -> Result<Box<dyn Tree + 'd>, Error> {
    let mut stream = VolumeStream::new(dev, first, sectors)?;
    let mut boot = [0; 512];
    stream.read_exact(&mut boot)?;
    io::Seek::rewind(&mut stream)?;
    if &boot[3..11] == b"EXFAT   " {
        Ok(Box::new(exfat::Exfat::open(stream)?))
    } else {
        Ok(Box::new(fat32::Fat32::open(stream)?))
    }
}

/// Stale files go first, so their directory slots and clusters are free for the new ones (hadris-fat never grows an
/// exFAT directory).
fn replace(tree: &mut dyn Tree, files: &[BootFile]) -> Result<Summary, Error> {
    let mut summary = Summary::default();
    for item in tree.list(BOOT_DIR)? {
        let path = format!("{BOOT_DIR}/{}", item.name);
        let wanted = files.iter().any(|f| f.path.eq_ignore_ascii_case(&path));
        if !item.dir && !wanted && !user_owned(&item.name) {
            tree.remove(&path)?;
            summary.removed.push(path);
        }
    }
    for f in files {
        if tree.read(f.path)?.as_deref() == Some(f.data) {
            summary.unchanged.push(f.path.to_string());
        } else {
            tree.write(f.path, f.data)?;
            summary.written.push(f.path.to_string());
        }
    }
    Ok(summary)
}
