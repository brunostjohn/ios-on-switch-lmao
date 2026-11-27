// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Read, Seek, SeekFrom, Write};

use switch_card::{Device, SECTOR};

const MAX_RUN_SECTORS: usize = 256;

/// One partition of a card as a byte stream, for the FAT crates: whole sectors go straight to the device, and a
/// partial one is read, patched and written back, so the device only ever sees sector-aligned transfers.
pub struct VolumeStream<'d, D: Device> {
    dev: &'d mut D,
    first: u64,
    len: u64,
    pos: u64,
}

impl<'d, D: Device> VolumeStream<'d, D> {
    pub fn new(dev: &'d mut D, first: u64, sectors: u64) -> io::Result<Self> {
        if first.checked_add(sectors).is_none_or(|end| end > dev.sector_count()) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "volume outside the card"));
        }
        Ok(VolumeStream {
            dev,
            first,
            len: sectors * SECTOR as u64,
            pos: 0,
        })
    }

    fn span(&self, want: usize) -> (u64, usize, usize) {
        let lba = self.first + self.pos / SECTOR as u64;
        let offset = (self.pos % SECTOR as u64) as usize;
        let n = want.min((self.len - self.pos) as usize);
        (lba, offset, n)
    }
}

impl<D: Device> Read for VolumeStream<'_, D> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.pos >= self.len || buf.is_empty() {
            return Ok(0);
        }
        let (lba, offset, n) = self.span(buf.len());
        let n = if offset == 0 && n >= SECTOR {
            let n = (n / SECTOR).min(MAX_RUN_SECTORS) * SECTOR;
            self.dev.read_sectors(lba, &mut buf[..n])?;
            n
        } else {
            let mut sector = [0; SECTOR];
            self.dev.read_sectors(lba, &mut sector)?;
            let n = n.min(SECTOR - offset);
            buf[..n].copy_from_slice(&sector[offset..offset + n]);
            n
        };
        self.pos += n as u64;
        Ok(n)
    }
}

impl<D: Device> Write for VolumeStream<'_, D> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        if self.pos >= self.len {
            return Err(io::Error::new(io::ErrorKind::WriteZero, "write past the volume's end"));
        }
        let (lba, offset, n) = self.span(buf.len());
        let n = if offset == 0 && n >= SECTOR {
            let n = (n / SECTOR).min(MAX_RUN_SECTORS) * SECTOR;
            self.dev.write_sectors(lba, &buf[..n])?;
            n
        } else {
            let mut sector = [0; SECTOR];
            self.dev.read_sectors(lba, &mut sector)?;
            let n = n.min(SECTOR - offset);
            sector[offset..offset + n].copy_from_slice(&buf[..n]);
            self.dev.write_sectors(lba, &sector)?;
            n
        };
        self.pos += n as u64;
        Ok(n)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.dev.barrier()
    }
}

impl<D: Device> Seek for VolumeStream<'_, D> {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let to = match pos {
            SeekFrom::Start(n) => Some(n),
            SeekFrom::End(d) => self.len.checked_add_signed(d),
            SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        match to {
            Some(n) if n <= self.len => {
                self.pos = n;
                Ok(n)
            }
            _ => Err(io::Error::new(io::ErrorKind::InvalidInput, "seek outside the volume")),
        }
    }
}
