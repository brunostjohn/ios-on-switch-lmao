// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("card I/O: {0}")]
    Io(#[from] io::Error),
    #[error("partition tables: {0}")]
    Card(#[from] switch_card::Error),
    #[error("master boot record: {0}")]
    Mbr(&'static str),
    #[error("journal (LBA 32752-32767): {0}")]
    Journal(&'static str),
    #[error("boot volume: {0}")]
    Volume(String),
    #[error("FAT damaged: {0}; repair it with chkdsk or fsck first")]
    Damaged(String),
    #[error("the boot volume's tail holds {need} clusters of files and its first part has {have} free")]
    NoRoom { need: u64, have: u64 },
    #[error("iOS size {bytes} bytes: {why}")]
    Size { bytes: u64, why: String },
    #[error("entry {0} is already named iOS-setup: install into it instead")]
    AlreadyPartitioned(u32),
    #[error("the card changed since the cut (LBA {lba}, {what}); it was used before the resume")]
    Changed { lba: u64, what: &'static str },
    #[error("the journal belongs to a different card or layout: {0}")]
    WrongCard(&'static str),
}

pub type Result<T> = std::result::Result<T, Error>;
