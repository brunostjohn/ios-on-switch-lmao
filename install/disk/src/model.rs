// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;

/// A whole disk as the OS reports it when listed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Disk {
    /// Names this disk until it is removed: macOS's IOKit registry entry ID, Linux's diskseq, Windows' device
    /// interface path. `open` refuses a disk whose id no longer matches.
    pub id: String,
    /// `/dev/disk4`, `/dev/sdb`, `\\.\PhysicalDrive2`.
    pub path: PathBuf,
    pub size: u64,
    pub logical_sector: u32,
    /// What the OS reports when listing; macOS lists the logical size here, and an open disk's own value is
    /// [`RawDisk::physical_sector`](crate::RawDisk::physical_sector).
    pub physical_sector: u32,
    pub removable: bool,
    pub internal: bool,
    /// Holds a volume the running OS needs (its root, boot, swap or system volumes).
    pub system: bool,
    pub read_only: bool,
    pub model: String,
    pub bus: Bus,
    pub partitions: Vec<Partition>,
    /// Volumes on the whole disk itself, with no partition table.
    pub volumes: Vec<Volume>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Bus {
    Usb,
    Sd,
    Sata,
    Nvme,
    Scsi,
    Virtual,
    Other(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Partition {
    pub path: PathBuf,
    pub number: u32,
    pub offset: u64,
    pub size: u64,
    pub name: String,
    /// The partition type: a GPT type GUID, an MBR type byte in hex, or macOS's content hint.
    pub kind: String,
    pub volumes: Vec<Volume>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Volume {
    pub mount_point: PathBuf,
    pub file_system: String,
}

impl Disk {
    pub fn mounted(&self) -> impl Iterator<Item = &Volume> {
        self.volumes
            .iter()
            .chain(self.partitions.iter().flat_map(|p| p.volumes.iter()))
    }
}
