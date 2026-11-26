// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{File, OpenOptions};
use std::io;
use std::path::Path;

use crate::object::{BLOCK_SIZE, Block};

/// Random-access 4 KiB reads by container block number.
pub trait BlockSource {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()>;

    fn block(&self, paddr: u64) -> io::Result<Box<Block>> {
        let mut out = Box::new([0u8; BLOCK_SIZE]);
        self.read_blocks(paddr, &mut out[..])?;
        Ok(out)
    }
}

pub trait BlockTarget {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()>;
    fn flush(&mut self) -> io::Result<()>;
}

/// A container inside a file or device, starting `base` bytes in. 21H461's sits at byte 17,408 of its GPT image,
/// which is not a multiple of the block size.
pub struct FileSource {
    file: File,
    base: u64,
}

impl FileSource {
    pub fn open(path: &Path, base: u64) -> io::Result<Self> {
        Ok(Self {
            file: File::open(path)?,
            base,
        })
    }
}

impl BlockSource for FileSource {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
        read_exact_at(&self.file, out, self.base + paddr * BLOCK_SIZE as u64)
    }
}

pub struct FileTarget {
    file: File,
}

impl FileTarget {
    /// A new image file of `bytes`, sparse where the filesystem allows: only written blocks take space.
    pub fn create(path: &Path, bytes: u64) -> io::Result<Self> {
        let file = OpenOptions::new().read(true).write(true).create_new(true).open(path)?;
        file.set_len(bytes)?;
        Ok(Self { file })
    }

    pub fn open(path: &Path) -> io::Result<Self> {
        Ok(Self {
            file: OpenOptions::new().read(true).write(true).open(path)?,
        })
    }

    pub fn len_blocks(&self) -> io::Result<u64> {
        Ok(self.file.metadata()?.len() / BLOCK_SIZE as u64)
    }
}

impl BlockTarget for FileTarget {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        write_all_at(&self.file, data, paddr * BLOCK_SIZE as u64)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.file.sync_data()
    }
}

impl BlockSource for FileTarget {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
        read_exact_at(&self.file, out, paddr * BLOCK_SIZE as u64)
    }
}

#[cfg(unix)]
fn read_exact_at(file: &File, out: &mut [u8], offset: u64) -> io::Result<()> {
    std::os::unix::fs::FileExt::read_exact_at(file, out, offset)
}

#[cfg(unix)]
fn write_all_at(file: &File, data: &[u8], offset: u64) -> io::Result<()> {
    std::os::unix::fs::FileExt::write_all_at(file, data, offset)
}

#[cfg(windows)]
fn read_exact_at(file: &File, mut out: &mut [u8], mut offset: u64) -> io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !out.is_empty() {
        match file.seek_read(out, offset)? {
            0 => return Err(io::ErrorKind::UnexpectedEof.into()),
            n => {
                out = &mut out[n..];
                offset += n as u64;
            }
        }
    }
    Ok(())
}

#[cfg(windows)]
fn write_all_at(file: &File, mut data: &[u8], mut offset: u64) -> io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !data.is_empty() {
        match file.seek_write(data, offset)? {
            0 => return Err(io::ErrorKind::WriteZero.into()),
            n => {
                data = &data[n..];
                offset += n as u64;
            }
        }
    }
    Ok(())
}
