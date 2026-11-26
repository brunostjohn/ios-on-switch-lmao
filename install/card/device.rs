// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};

/// A card, or an image of one, addressed in sectors.
pub trait Device {
    fn sector_size(&self) -> u32;
    fn sector_count(&self) -> u64;
    /// `buf` is a whole number of sectors.
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()>;
    /// `buf` is a whole number of sectors.
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()>;
    /// Returns once every earlier write is on the medium, so no later write can land before it.
    fn barrier(&mut self) -> io::Result<()>;
}

impl<D: Device + ?Sized> Device for &mut D {
    fn sector_size(&self) -> u32 {
        (**self).sector_size()
    }
    fn sector_count(&self) -> u64 {
        (**self).sector_count()
    }
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        (**self).read_sectors(lba, buf)
    }
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        (**self).write_sectors(lba, buf)
    }
    fn barrier(&mut self) -> io::Result<()> {
        (**self).barrier()
    }
}

/// A `Read + Write + Seek` stream as a device: an image file, or anything else byte-addressed.
pub struct StreamDevice<T> {
    stream: T,
    sector_size: u32,
    sectors: u64,
    barrier: fn(&mut T) -> io::Result<()>,
}

impl<T: Read + Write + Seek> StreamDevice<T> {
    pub fn new(stream: T, sector_size: u32, sectors: u64, barrier: fn(&mut T) -> io::Result<()>) -> Self {
        StreamDevice {
            stream,
            sector_size,
            sectors,
            barrier,
        }
    }

    pub fn into_inner(self) -> T {
        self.stream
    }

    fn seek_to(&mut self, lba: u64, len: usize) -> io::Result<()> {
        let size = u64::from(self.sector_size);
        if len as u64 % size != 0 || lba.checked_add(len as u64 / size).is_none_or(|end| end > self.sectors) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "transfer outside the device",
            ));
        }
        self.stream.seek(SeekFrom::Start(lba * size)).map(drop)
    }
}

impl StreamDevice<File> {
    /// An image file, its length taken as the card's; `barrier` is `fsync`'s data half.
    pub fn file(file: File, sector_size: u32) -> io::Result<Self> {
        let sectors = file.metadata()?.len() / u64::from(sector_size);
        Ok(StreamDevice::new(file, sector_size, sectors, |f| f.sync_data()))
    }
}

impl<T: Read + Write + Seek> Device for StreamDevice<T> {
    fn sector_size(&self) -> u32 {
        self.sector_size
    }
    fn sector_count(&self) -> u64 {
        self.sectors
    }
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        self.seek_to(lba, buf.len())?;
        self.stream.read_exact(buf)
    }
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        self.seek_to(lba, buf.len())?;
        self.stream.write_all(buf)
    }
    fn barrier(&mut self) -> io::Result<()> {
        self.stream.flush()?;
        (self.barrier)(&mut self.stream)
    }
}
