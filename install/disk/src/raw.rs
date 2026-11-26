// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io;

use crate::align::{AlignedBuf, Piece, align_down, is_aligned, split};
use crate::error::{Error, Result};
use crate::model::Disk;
use crate::platform;

/// Misaligned caller memory goes down through a staging buffer this large at a time.
const STAGE: usize = 8 << 20;

#[derive(Clone, Copy, Debug)]
pub(crate) struct Geometry {
    pub size: u64,
    pub logical: u32,
    pub physical: u32,
}

/// A whole disk opened for exclusive, uncached, sector-aligned access. Its volumes are unmounted and the OS mounts
/// nothing from it until it is released (or dropped) or ejected.
///
/// Reads and writes take any offset and length: whole sectors go straight to the device, and a partial sector at
/// either end is read, patched and written back. Two writes running at once must not share a sector.
pub struct RawDisk {
    file: Option<File>,
    hold: Option<platform::Hold>,
    disk: Disk,
    geometry: Geometry,
    align: usize,
}

impl RawDisk {
    pub(crate) fn new(disk: Disk, file: File, geometry: Geometry, hold: platform::Hold) -> Self {
        let align = (geometry.physical.max(geometry.logical) as usize).max(4096);
        RawDisk {
            file: Some(file),
            hold: Some(hold),
            disk,
            geometry,
            align,
        }
    }

    pub fn disk(&self) -> &Disk {
        &self.disk
    }

    pub fn size(&self) -> u64 {
        self.geometry.size
    }

    pub fn logical_sector(&self) -> u32 {
        self.geometry.logical
    }

    pub fn physical_sector(&self) -> u32 {
        self.geometry.physical
    }

    /// The memory alignment a buffer needs to go to the device without a copy.
    pub fn alignment(&self) -> usize {
        self.align
    }

    pub fn buffer(&self, len: usize) -> AlignedBuf {
        AlignedBuf::new(len, self.align)
    }

    pub fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        self.check("read", offset, buf.len())?;
        let parts = split(offset, buf.len(), self.geometry.logical);
        for edge in [parts.head, parts.tail].into_iter().flatten() {
            let sector = self.read_sector(edge.offset)?;
            let at = (edge.offset % u64::from(self.geometry.logical)) as usize;
            buf[edge.data.clone()].copy_from_slice(&sector[at..at + edge.data.len()]);
        }
        if let Some(body) = parts.body {
            self.read_body(&body, &mut buf[body.data.clone()])?;
        }
        Ok(())
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<()> {
        self.check("write", offset, data.len())?;
        let parts = split(offset, data.len(), self.geometry.logical);
        for edge in [parts.head, parts.tail].into_iter().flatten() {
            let mut sector = self.read_sector(edge.offset)?;
            let at = (edge.offset % u64::from(self.geometry.logical)) as usize;
            sector[at..at + edge.data.len()].copy_from_slice(&data[edge.data.clone()]);
            self.pwrite(&sector, align_down(edge.offset, u64::from(self.geometry.logical)))?;
        }
        if let Some(body) = parts.body {
            self.write_body(&body, &data[body.data.clone()])?;
        }
        Ok(())
    }

    /// Pushes everything written so far out of the device's own cache onto the medium.
    pub fn flush(&self) -> Result<()> {
        platform::flush(self.file()).map_err(|e| Error::io(format!("flush {}", self.disk.path.display()), e))
    }

    /// Lets the OS see the disk again: it re-reads the partition table and mounts what was mounted before `open`.
    pub fn release(mut self) -> Result<()> {
        let (file, hold) = self.take();
        hold.release(file)
    }

    /// Ejects the disk with the OS still kept from mounting it, so nothing touches the new volumes on the way out.
    pub fn eject(mut self) -> Result<()> {
        let (file, hold) = self.take();
        hold.eject(file)
    }

    fn take(&mut self) -> (File, platform::Hold) {
        (
            self.file.take().expect("open until released"),
            self.hold.take().expect("held until released"),
        )
    }

    fn file(&self) -> &File {
        self.file.as_ref().expect("open until released")
    }

    fn check(&self, op: &'static str, offset: u64, len: usize) -> Result<()> {
        let size = self.geometry.size;
        match offset.checked_add(len as u64) {
            Some(end) if end <= size => Ok(()),
            _ => Err(Error::OutOfRange {
                op,
                offset,
                len: len as u64,
                size,
            }),
        }
    }

    fn read_sector(&self, offset: u64) -> Result<AlignedBuf> {
        let sector = u64::from(self.geometry.logical);
        let mut buf = self.buffer(sector as usize);
        self.pread(&mut buf, align_down(offset, sector))?;
        Ok(buf)
    }

    fn read_body(&self, body: &Piece, out: &mut [u8]) -> Result<()> {
        if is_aligned(out.as_ptr(), self.align) {
            return self.pread(out, body.offset);
        }
        let mut stage = self.buffer(out.len().min(STAGE));
        for (i, chunk) in out.chunks_mut(STAGE).enumerate() {
            let staged = &mut stage[..chunk.len()];
            self.pread(staged, body.offset + (i * STAGE) as u64)?;
            chunk.copy_from_slice(staged);
        }
        Ok(())
    }

    fn write_body(&self, body: &Piece, data: &[u8]) -> Result<()> {
        if is_aligned(data.as_ptr(), self.align) {
            return self.pwrite(data, body.offset);
        }
        let mut stage = self.buffer(data.len().min(STAGE));
        for (i, chunk) in data.chunks(STAGE).enumerate() {
            stage[..chunk.len()].copy_from_slice(chunk);
            self.pwrite(&stage[..chunk.len()], body.offset + (i * STAGE) as u64)?;
        }
        Ok(())
    }

    fn pread(&self, buf: &mut [u8], offset: u64) -> Result<()> {
        positional::read_exact_at(self.file(), buf, offset).map_err(|e| self.io_error("read", offset, e))
    }

    fn pwrite(&self, buf: &[u8], offset: u64) -> Result<()> {
        positional::write_all_at(self.file(), buf, offset).map_err(|e| self.io_error("write", offset, e))
    }

    fn io_error(&self, op: &str, offset: u64, e: io::Error) -> Error {
        Error::io(format!("{op} {} at {offset}", self.disk.path.display()), e)
    }
}

impl Drop for RawDisk {
    fn drop(&mut self) {
        if let (Some(file), Some(hold)) = (self.file.take(), self.hold.take()) {
            let _ = hold.release(file);
        }
    }
}

#[cfg(unix)]
mod positional {
    use std::fs::File;
    use std::io;
    use std::os::unix::fs::FileExt;

    pub fn read_exact_at(file: &File, buf: &mut [u8], offset: u64) -> io::Result<()> {
        file.read_exact_at(buf, offset)
    }

    pub fn write_all_at(file: &File, buf: &[u8], offset: u64) -> io::Result<()> {
        file.write_all_at(buf, offset)
    }
}

#[cfg(windows)]
mod positional {
    use std::fs::File;
    use std::io;
    use std::os::windows::fs::FileExt;

    pub fn read_exact_at(file: &File, mut buf: &mut [u8], mut offset: u64) -> io::Result<()> {
        while !buf.is_empty() {
            match file.seek_read(buf, offset)? {
                0 => return Err(io::ErrorKind::UnexpectedEof.into()),
                n => {
                    buf = &mut buf[n..];
                    offset += n as u64;
                }
            }
        }
        Ok(())
    }

    pub fn write_all_at(file: &File, mut buf: &[u8], mut offset: u64) -> io::Result<()> {
        while !buf.is_empty() {
            match file.seek_write(buf, offset)? {
                0 => return Err(io::ErrorKind::WriteZero.into()),
                n => {
                    buf = &buf[n..];
                    offset += n as u64;
                }
            }
        }
        Ok(())
    }
}
