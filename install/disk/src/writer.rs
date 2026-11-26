// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;

use crate::align::{AlignedBuf, align_down, align_up};
use crate::error::Result;
use crate::raw::RawDisk;

const CHUNK: usize = 8 << 20;

/// A sequential writer from any offset, for callers that write in small or unaligned pieces. It gathers them into
/// whole-sector chunks of 8 MiB; a run at least that long goes straight down without the copy.
///
/// [`io::Write::flush`] writes out what is gathered, padding a partial last sector with the bytes already on the disk.
/// It does not flush the device: [`RawDisk::flush`] does.
pub struct Writer<'a> {
    disk: &'a RawDisk,
    buf: AlignedBuf,
    base: u64,
    filled: usize,
}

impl<'a> Writer<'a> {
    pub(crate) fn new(disk: &'a RawDisk, offset: u64) -> Result<Self> {
        let sector = u64::from(disk.logical_sector());
        let base = align_down(offset, sector);
        let mut buf = disk.buffer(CHUNK);
        let lead = (offset - base) as usize;
        if lead > 0 {
            disk.read_at(base, &mut buf[..sector as usize])?;
        }
        Ok(Writer {
            disk,
            buf,
            base,
            filled: lead,
        })
    }

    pub fn position(&self) -> u64 {
        self.base + self.filled as u64
    }

    /// Writes out what is gathered and returns the offset just past the last byte written.
    pub fn finish(mut self) -> Result<u64> {
        self.write_gathered()?;
        let end = self.position();
        self.filled = 0;
        Ok(end)
    }

    fn put(&mut self, mut data: &[u8]) -> Result<()> {
        let sector = self.disk.logical_sector() as usize;
        while !data.is_empty() {
            if self.filled == 0 && data.len() >= CHUNK {
                let run = data.len() - data.len() % sector;
                self.disk.write_at(self.base, &data[..run])?;
                self.base += run as u64;
                data = &data[run..];
                continue;
            }
            let take = (CHUNK - self.filled).min(data.len());
            self.buf[self.filled..self.filled + take].copy_from_slice(&data[..take]);
            self.filled += take;
            data = &data[take..];
            if self.filled == CHUNK {
                self.disk.write_at(self.base, &self.buf)?;
                self.base += CHUNK as u64;
                self.filled = 0;
            }
        }
        Ok(())
    }

    /// The buffer keeps its contents, so later writes keep appending to the same sectors.
    fn write_gathered(&mut self) -> Result<()> {
        if self.filled == 0 {
            return Ok(());
        }
        let sector = u64::from(self.disk.logical_sector());
        let padded = align_up(self.filled as u64, sector) as usize;
        if padded != self.filled {
            let last = align_down(self.filled as u64, sector) as usize;
            let mut existing = self.disk.buffer(sector as usize);
            self.disk.read_at(self.base + last as u64, &mut existing)?;
            let keep = self.filled - last;
            self.buf[self.filled..padded].copy_from_slice(&existing[keep..]);
        }
        self.disk.write_at(self.base, &self.buf[..padded])
    }
}

impl io::Write for Writer<'_> {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.put(data).map_err(io::Error::other)?;
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.write_gathered().map_err(io::Error::other)
    }
}

impl Drop for Writer<'_> {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let _ = self.write_gathered();
        }
    }
}

impl RawDisk {
    pub fn writer(&self, offset: u64) -> Result<Writer<'_>> {
        Writer::new(self, offset)
    }
}
