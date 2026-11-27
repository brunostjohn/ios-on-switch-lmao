// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("card I/O: {0}")]
    Io(#[from] io::Error),
    #[error("{0}-byte sectors: the kernel reads cards in 512-byte sectors only")]
    SectorSize(u32),
    #[error("a card of {0} sectors is too small for a GPT")]
    TooSmall(u64),
    #[error("master boot record: {0}")]
    Mbr(String),
    #[error("the card has no GPT (no protective 0xEE entry in its MBR): partition it first")]
    NoGpt,
    #[error("{} GPT header: {why}", if *.backup { "backup" } else { "primary" })]
    Header { backup: bool, why: &'static str },
    #[error("GPT entries of {0} bytes: the kernel's strict scan reads 128, 256 or 512")]
    EntrySize(u32),
    #[error("GPT entry {index}: {why}")]
    Entry { index: u32, why: &'static str },
    #[error("the GPT's two copies differ: {0}")]
    CopiesDiffer(&'static str),
    #[error("no partition to install into")]
    NoTarget,
    #[error("more than one partition could be the install's ({0:?}): choose one")]
    Ambiguous(Vec<u32>),
    #[error("GPT entry {0} is neither iOS, iOS-setup nor a userdata, l4t or UDA Linux partition")]
    NotATarget(u32),
    #[error("GPT entry {index} reaches past LBA 2^32 - 1, where the kernel cannot address it")]
    Unaddressable { index: u32 },
    #[error("GPT entry {index} holds {have} bytes; the image needs {need}")]
    TooSmallForImage { index: u32, have: u64, need: u64 },
    #[error("GPT entry {0} counts as an iOS root but is the FAT, a data partition or emuMMC: rename it first")]
    ForeignRoot(u32),
    #[error("no FAT boot volume: {0}")]
    NoBootVolume(&'static str),
}
