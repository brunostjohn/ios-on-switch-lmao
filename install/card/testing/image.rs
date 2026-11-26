// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io;

use switch_card::{Device, SECTOR};

pub type Sector = [u8; SECTOR];

/// A card image of any size in memory: sectors never written read as zeros.
#[derive(Clone, PartialEq, Eq)]
pub struct SparseImage {
    pub sectors: u64,
    pub data: BTreeMap<u64, Box<Sector>>,
}

impl SparseImage {
    pub fn new(sectors: u64) -> SparseImage {
        SparseImage {
            sectors,
            data: BTreeMap::new(),
        }
    }

    pub fn sector(&self, lba: u64) -> Sector {
        self.data.get(&lba).map_or([0; SECTOR], |s| **s)
    }

    pub fn set(&mut self, lba: u64, bytes: &[u8]) {
        for (i, chunk) in bytes.chunks(SECTOR).enumerate() {
            let mut sector = [0; SECTOR];
            sector[..chunk.len()].copy_from_slice(chunk);
            self.data.insert(lba + i as u64, Box::new(sector));
        }
    }

    pub fn read_bytes(&self, lba: u64, sectors: u64) -> Vec<u8> {
        (lba..lba + sectors).flat_map(|s| self.sector(s)).collect()
    }
}

impl Device for SparseImage {
    fn sector_size(&self) -> u32 {
        SECTOR as u32
    }
    fn sector_count(&self) -> u64 {
        self.sectors
    }
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        check(self.sectors, lba, buf.len())?;
        for (i, chunk) in buf.chunks_mut(SECTOR).enumerate() {
            chunk.copy_from_slice(&self.sector(lba + i as u64));
        }
        Ok(())
    }
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        check(self.sectors, lba, buf.len())?;
        self.set(lba, buf);
        Ok(())
    }
    fn barrier(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn check(sectors: u64, lba: u64, len: usize) -> io::Result<()> {
    if len % SECTOR != 0 || lba + (len / SECTOR) as u64 > sectors {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "transfer outside the image",
        ));
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Write(u64, Vec<u8>),
    Barrier,
}

/// A device that keeps every write and barrier it passes on, for replaying any prefix of them: a power cut there.
pub struct Recorder<'a, D: Device> {
    pub inner: &'a mut D,
    pub events: Vec<Event>,
}

impl<'a, D: Device> Recorder<'a, D> {
    pub fn new(inner: &'a mut D) -> Self {
        Recorder {
            inner,
            events: Vec::new(),
        }
    }

    pub fn writes(&self) -> Vec<(u64, Vec<u8>)> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Event::Write(lba, data) => Some((*lba, data.clone())),
                Event::Barrier => None,
            })
            .collect()
    }
}

impl<D: Device> Device for Recorder<'_, D> {
    fn sector_size(&self) -> u32 {
        self.inner.sector_size()
    }
    fn sector_count(&self) -> u64 {
        self.inner.sector_count()
    }
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        self.inner.read_sectors(lba, buf)
    }
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        self.events.push(Event::Write(lba, buf.to_vec()));
        self.inner.write_sectors(lba, buf)
    }
    fn barrier(&mut self) -> io::Result<()> {
        self.events.push(Event::Barrier);
        self.inner.barrier()
    }
}
