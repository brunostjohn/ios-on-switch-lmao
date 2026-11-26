// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT through hadris-fat's preview, which takes the root directory as one cluster and the allocation bitmap and
//! up-case table as unfragmented (hadris-fat 2.5.0 src/exfat/fs.rs `open`). The volume is checked for exactly that
//! first, so a card it would misread is refused before anything is written. So is a directory on the boot entry's
//! paths longer than one cluster: hadris never makes one, and no test can (a Buildbarn worker attaches images but
//! cannot mount them), so that path of its reader is never taken.

use std::io::{Read, Seek, SeekFrom};

use hadris_fat::exfat::{ExFatDir, ExFatVolume};
use switch_card::Device;

use crate::stream::VolumeStream;
use crate::{Error, Item, Tree};

const END_OF_CHAIN: u32 = 0xffff_ffff;
const BITMAP_ENTRY: u8 = 0x81;
const UPCASE_ENTRY: u8 = 0x82;
const END_OF_DIRECTORY: u8 = 0x00;

pub struct Exfat<'d, D: Device> {
    vol: ExFatVolume<VolumeStream<'d, D>>,
}

impl<'d, D: Device> Exfat<'d, D> {
    pub fn open(mut stream: VolumeStream<'d, D>) -> Result<Self, Error> {
        let layout = Layout::read(&mut stream)?;
        layout.check(&mut stream)?;
        stream.seek(SeekFrom::Start(0))?;
        let vol = ExFatVolume::open(stream).map_err(exfat)?;
        for dir in crate::allow::directories() {
            match vol.open_path(dir) {
                Ok(e) if e.is_directory() && e.data_length > layout.cluster_bytes() => {
                    return Err(Error::Unsupported(
                        "an exFAT directory of the boot entry longer than one cluster",
                    ));
                }
                Ok(_) | Err(hadris_fat::Error::EntryNotFound) => {}
                Err(e) => return Err(exfat(e)),
            }
        }
        Ok(Exfat { vol })
    }

    fn dir(&self, path: &str) -> Result<Option<ExFatDir<'_, VolumeStream<'d, D>>>, Error> {
        match self.vol.open_dir(path) {
            Ok(dir) => Ok(Some(dir)),
            Err(hadris_fat::Error::EntryNotFound) => Ok(None),
            Err(e) => Err(exfat(e)),
        }
    }
}

fn exfat(e: hadris_fat::Error) -> Error {
    Error::Exfat(e.to_string())
}

fn io(e: hadris_fat::io::Error) -> Error {
    Error::Exfat(e.to_string())
}

struct Layout {
    sector_shift: u32,
    cluster_shift: u32,
    fat_offset: u64,
    heap_offset: u64,
    root: u32,
}

impl Layout {
    fn read<S: Read + Seek>(s: &mut S) -> Result<Layout, Error> {
        let mut boot = [0; 512];
        s.seek(SeekFrom::Start(0))?;
        s.read_exact(&mut boot)?;
        let le32 = |at: usize| u32::from_le_bytes(boot[at..at + 4].try_into().unwrap());
        Ok(Layout {
            sector_shift: u32::from(boot[108]),
            cluster_shift: u32::from(boot[109]),
            fat_offset: u64::from(le32(80)),
            heap_offset: u64::from(le32(88)),
            root: le32(96),
        })
    }

    fn fat_entry<S: Read + Seek>(&self, s: &mut S, cluster: u32) -> Result<u32, Error> {
        let mut entry = [0; 4];
        s.seek(SeekFrom::Start(
            (self.fat_offset << self.sector_shift) + u64::from(cluster) * 4,
        ))?;
        s.read_exact(&mut entry)?;
        Ok(u32::from_le_bytes(entry))
    }

    fn cluster_bytes(&self) -> u64 {
        1 << (self.sector_shift + self.cluster_shift)
    }

    fn cluster_offset(&self, cluster: u32) -> u64 {
        (self.heap_offset << self.sector_shift) + u64::from(cluster - 2) * self.cluster_bytes()
    }

    /// The root one cluster long, and the bitmap and up-case table each in consecutive clusters.
    fn check<S: Read + Seek>(&self, s: &mut S) -> Result<(), Error> {
        if !(9..=12).contains(&self.sector_shift) || self.cluster_shift > 25 - self.sector_shift || self.root < 2 {
            return Err(Error::Unsupported("an exFAT boot sector out of range"));
        }
        if self.fat_entry(s, self.root)? != END_OF_CHAIN {
            return Err(Error::Unsupported("an exFAT root directory longer than one cluster"));
        }
        let mut entries = vec![0; self.cluster_bytes() as usize];
        s.seek(SeekFrom::Start(self.cluster_offset(self.root)))?;
        s.read_exact(&mut entries)?;
        for entry in entries.chunks(32) {
            match entry[0] {
                END_OF_DIRECTORY => break,
                BITMAP_ENTRY | UPCASE_ENTRY => {
                    let first = u32::from_le_bytes(entry[20..24].try_into().unwrap());
                    let length = u64::from_le_bytes(entry[24..32].try_into().unwrap());
                    self.check_run(s, first, length.div_ceil(self.cluster_bytes()))?;
                }
                _ => {}
            }
        }
        Ok(())
    }

    fn check_run<S: Read + Seek>(&self, s: &mut S, first: u32, clusters: u64) -> Result<(), Error> {
        for i in 0..clusters {
            let cluster = first + i as u32;
            let next = self.fat_entry(s, cluster)?;
            let want = if i + 1 == clusters { END_OF_CHAIN } else { cluster + 1 };
            if next != want {
                return Err(Error::Unsupported(
                    "a fragmented exFAT allocation bitmap or up-case table",
                ));
            }
        }
        Ok(())
    }
}

impl<D: Device> Tree for Exfat<'_, D> {
    fn read(&mut self, path: &str) -> Result<Option<Vec<u8>>, Error> {
        let mut reader = match self.vol.open_file(path) {
            Ok(r) => r,
            Err(hadris_fat::Error::EntryNotFound) => return Ok(None),
            Err(e) => return Err(exfat(e)),
        };
        let mut data = vec![0; reader.size() as usize];
        hadris_fat::io::Read::read_exact(&mut reader, &mut data).map_err(io)?;
        Ok(Some(data))
    }

    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), Error> {
        let (parent, name) = path.rsplit_once('/').unwrap();
        let mut dir = self.vol.root_dir();
        for component in parent.split('/') {
            dir = match dir.find(component).map_err(exfat)? {
                Some(e) if e.is_directory() => dir.open_dir(component).map_err(exfat)?,
                Some(_) => return Err(Error::Exfat(format!("{component} is a file, not a directory"))),
                None => self.vol.create_dir(&dir, component).map_err(exfat)?,
            };
        }
        if let Some(old) = dir.find(name).map_err(exfat)? {
            self.vol.delete(&old).map_err(exfat)?;
        }
        let entry = self.vol.create_file(&dir, name).map_err(exfat)?;
        let mut writer = self.vol.write_file(&entry).map_err(exfat)?;
        hadris_fat::io::Write::write_all(&mut writer, data).map_err(io)?;
        writer.finish().map_err(exfat)
    }

    fn list(&mut self, path: &str) -> Result<Vec<Item>, Error> {
        let Some(dir) = self.dir(path)? else {
            return Ok(Vec::new());
        };
        dir.entries()
            .map(|e| {
                e.map(|e| Item {
                    dir: e.is_directory(),
                    name: e.name,
                })
                .map_err(exfat)
            })
            .collect()
    }

    fn remove(&mut self, path: &str) -> Result<(), Error> {
        let entry = self.vol.open_path(path).map_err(exfat)?;
        self.vol.delete(&entry).map_err(exfat)
    }

    fn close(self: Box<Self>) -> Result<(), Error> {
        self.vol.sync_bitmap().map_err(exfat)
    }
}
