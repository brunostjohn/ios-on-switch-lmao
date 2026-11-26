// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::model::Bus;

/// sysfs sizes and offsets count 512-byte units whatever the device's sector size.
const UNIT: u64 = 512;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BlockDevice {
    pub name: String,
    pub device: (u32, u32),
    pub size: u64,
    pub logical: u32,
    pub physical: u32,
    pub removable: bool,
    pub read_only: bool,
    pub model: String,
    pub diskseq: Option<u64>,
    pub bus: Bus,
    pub partitions: Vec<BlockPartition>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BlockPartition {
    pub name: String,
    pub device: (u32, u32),
    pub number: u32,
    pub offset: u64,
    pub size: u64,
    pub label: String,
}

/// Every whole disk under `<sys>/block` that can hold a card or an image: device-mapper, md, RAM, optical and
/// floppy devices are left out, and so are loop devices with nothing attached.
pub fn read_block_devices(sys: &Path) -> io::Result<Vec<BlockDevice>> {
    let mut devices = Vec::new();
    for entry in fs::read_dir(sys.join("block"))? {
        let name = entry?.file_name().to_string_lossy().into_owned();
        if ["dm-", "md", "ram", "zram", "sr", "fd"]
            .iter()
            .any(|p| name.starts_with(p))
        {
            continue;
        }
        let device = read_device(sys, &name)?;
        if name.starts_with("loop") && device.size == 0 {
            continue;
        }
        devices.push(device);
    }
    devices.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(devices)
}

fn read_device(sys: &Path, name: &str) -> io::Result<BlockDevice> {
    let dir = sys.join("block").join(name);
    let resolved = fs::canonicalize(&dir).unwrap_or_else(|_| dir.clone());
    let card_type = read_trimmed(&dir.join("device/type")).unwrap_or_default();
    let bus = bus_of(&resolved.to_string_lossy(), name, &card_type);
    let removable = read_trimmed(&dir.join("removable")).as_deref() == Some("1") || bus == Bus::Sd;
    let mut partitions = Vec::new();
    for entry in fs::read_dir(&dir)? {
        let entry = entry?;
        if entry.path().join("partition").is_file() {
            partitions.push(read_partition(&entry.path())?);
        }
    }
    partitions.sort_by_key(|p| p.number);
    Ok(BlockDevice {
        name: name.to_owned(),
        device: read_dev(&dir.join("dev"))?,
        size: read_u64(&dir.join("size"))? * UNIT,
        logical: read_u64(&dir.join("queue/logical_block_size")).unwrap_or(UNIT) as u32,
        physical: read_u64(&dir.join("queue/physical_block_size")).unwrap_or(UNIT) as u32,
        removable,
        read_only: read_trimmed(&dir.join("ro")).as_deref() == Some("1"),
        model: model_of(&dir),
        diskseq: read_u64(&dir.join("diskseq")).ok(),
        bus,
        partitions,
    })
}

fn read_partition(dir: &Path) -> io::Result<BlockPartition> {
    let uevent = fs::read_to_string(dir.join("uevent")).unwrap_or_default();
    let label = uevent
        .lines()
        .find_map(|l| l.strip_prefix("PARTNAME="))
        .unwrap_or_default()
        .to_owned();
    Ok(BlockPartition {
        name: dir.file_name().unwrap_or_default().to_string_lossy().into_owned(),
        device: read_dev(&dir.join("dev"))?,
        number: read_u64(&dir.join("partition"))? as u32,
        offset: read_u64(&dir.join("start"))? * UNIT,
        size: read_u64(&dir.join("size"))? * UNIT,
        label,
    })
}

/// The devices `device` is built on (a dm-crypt or md device's members), down to the partitions or disks at the
/// bottom, through `<sys>/dev/block/<major>:<minor>/slaves`.
pub fn underlying(sys: &Path, device: (u32, u32)) -> Vec<(u32, u32)> {
    let slaves = sys.join(format!("dev/block/{}:{}/slaves", device.0, device.1));
    let Ok(entries) = fs::read_dir(&slaves) else {
        return vec![device];
    };
    let mut found = Vec::new();
    for entry in entries.flatten() {
        if let Ok(slave) = read_dev(&entry.path().join("dev")) {
            found.extend(underlying(sys, slave));
        }
    }
    if found.is_empty() { vec![device] } else { found }
}

/// The bus from where the device sits in `/sys/devices`. `card_type` is an MMC device's `type`: `SD` for a card,
/// `MMC` for soldered eMMC.
pub fn bus_of(resolved: &str, name: &str, card_type: &str) -> Bus {
    if resolved.contains("/usb") {
        Bus::Usb
    } else if name.starts_with("mmcblk") {
        if card_type == "SD" {
            Bus::Sd
        } else {
            Bus::Other("eMMC".into())
        }
    } else if name.starts_with("nvme") {
        Bus::Nvme
    } else if resolved.contains("/virtual/") {
        Bus::Virtual
    } else if resolved.contains("/ata") {
        Bus::Sata
    } else if resolved.contains("/host") || resolved.contains("/scsi") {
        Bus::Scsi
    } else {
        Bus::Other(String::new())
    }
}

fn model_of(dir: &Path) -> String {
    let part = |p: &str| read_trimmed(&dir.join(p)).unwrap_or_default();
    let words = [part("device/vendor"), part("device/model"), part("device/name")];
    words
        .into_iter()
        .filter(|w| !w.is_empty())
        .collect::<Vec<_>>()
        .join(" ")
}

pub fn parse_dev(text: &str) -> Option<(u32, u32)> {
    let (major, minor) = text.trim().split_once(':')?;
    Some((major.parse().ok()?, minor.parse().ok()?))
}

fn read_dev(path: &Path) -> io::Result<(u32, u32)> {
    let text = fs::read_to_string(path)?;
    parse_dev(&text).ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, format!("{}: {text}", path.display())))
}

fn read_trimmed(path: &Path) -> Option<String> {
    fs::read_to_string(path).ok().map(|s| s.trim().to_owned())
}

fn read_u64(path: &Path) -> io::Result<u64> {
    let text = fs::read_to_string(path)?;
    text.trim()
        .parse()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("{}: {text}", path.display())))
}

pub fn dev_path(name: &str) -> PathBuf {
    Path::new("/dev").join(name)
}
