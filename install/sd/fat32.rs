// SPDX-License-Identifier: GPL-3.0-or-later
//! FAT32 through hadris-fat. (fatfs 0.3.6 writes a first-level directory's `..` as the root's cluster rather than 0,
//! dir.rs:249-253, and `fsck_msdos -n` rejects every directory it makes.)

use hadris_fat::{FatDir, FatType, FatVolume, FatVolumeWriteExt};
use switch_card::Device;

use crate::stream::VolumeStream;
use crate::{Error, Item, Tree};

pub struct Fat32<'d, D: Device> {
    vol: FatVolume<VolumeStream<'d, D>>,
}

type Dir<'v, 'd, D> = FatDir<'v, VolumeStream<'d, D>>;

impl<'d, D: Device> Fat32<'d, D> {
    pub fn open(stream: VolumeStream<'d, D>) -> Result<Self, Error> {
        let vol = FatVolume::open(stream).map_err(fat)?;
        if vol.fat_type() != FatType::Fat32 {
            return Err(Error::Unsupported(
                "a FAT12 or FAT16 volume: hekate and the kernel read FAT32 or exFAT",
            ));
        }
        Ok(Fat32 { vol })
    }

    /// The directory at `path`, made as needed when `create`; None when it is missing and not `create`.
    fn dir(&self, path: &str, create: bool) -> Result<Option<Dir<'_, 'd, D>>, Error> {
        let mut dir = self.vol.root_dir();
        for component in path.split('/') {
            dir = match dir.find(component).map_err(fat)? {
                Some(e) if e.is_directory() => dir.open_dir(component).map_err(fat)?,
                Some(_) => return Err(Error::Fat(format!("{component} is a file, not a directory"))),
                None if create => self.vol.create_dir(&dir, component).map_err(fat)?,
                None => return Ok(None),
            };
        }
        Ok(Some(dir))
    }
}

fn fat(e: hadris_fat::Error) -> Error {
    Error::Fat(e.to_string())
}

impl<D: Device> Tree for Fat32<'_, D> {
    fn read(&mut self, path: &str) -> Result<Option<Vec<u8>>, Error> {
        let (parent, name) = path.rsplit_once('/').unwrap();
        let Some(dir) = self.dir(parent, false)? else {
            return Ok(None);
        };
        match dir.find(name).map_err(fat)? {
            Some(e) if !e.is_directory() => dir.open_file(name).map_err(fat)?.read_to_vec().map(Some).map_err(fat),
            _ => Ok(None),
        }
    }

    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), Error> {
        let (parent, name) = path.rsplit_once('/').unwrap();
        let dir = self.dir(parent, true)?.unwrap();
        if let Some(old) = dir.find(name).map_err(fat)? {
            self.vol.delete(&old).map_err(fat)?;
        }
        let entry = self.vol.create_file(&dir, name).map_err(fat)?;
        let mut writer = self.vol.write_file(&entry).map_err(fat)?;
        let mut rest = data;
        while !rest.is_empty() {
            match writer.write(rest).map_err(fat)? {
                0 => return Err(Error::Fat(format!("{path}: the volume is full"))),
                n => rest = &rest[n..],
            }
        }
        writer.finish().map_err(fat)
    }

    fn list(&mut self, path: &str) -> Result<Vec<Item>, Error> {
        let Some(dir) = self.dir(path, false)? else {
            return Ok(Vec::new());
        };
        let mut items = Vec::new();
        for entry in dir.entries() {
            let entry = entry.map_err(fat)?;
            let Some(e) = entry.as_entry() else { continue };
            let name = e.name().into_owned();
            if name != "." && name != ".." {
                items.push(Item {
                    dir: e.is_directory(),
                    name,
                });
            }
        }
        Ok(items)
    }

    fn remove(&mut self, path: &str) -> Result<(), Error> {
        let entry = self.vol.open_path(path).map_err(fat)?;
        self.vol.delete(&entry).map_err(fat)
    }

    fn close(self: Box<Self>) -> Result<(), Error> {
        self.vol.sync().map_err(fat)
    }
}
