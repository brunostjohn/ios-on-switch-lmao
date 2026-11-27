// SPDX-License-Identifier: GPL-3.0-or-later
//! O_EXCL on a whole disk claims it: the kernel then refuses to mount any of its partitions until the descriptor
//! closes, and the open itself fails while one is mounted. Volumes are unmounted first, by umount(2) as root or
//! through udisks otherwise. After the close the partition table is read again and the desktop's automounter
//! mounts what it wants.
mod udisks;

use std::collections::HashSet;
use std::ffi::CString;
use std::fs::{self, File, OpenOptions as FsOpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt};
use std::path::Path;

use crate::error::{Error, Result};
use crate::model::{Bus, Disk, Partition, Volume};
use crate::parse::mounts::{Mount, parse_mountinfo, parse_swaps};
use crate::parse::sysfs::{BlockDevice, dev_path, read_block_devices, underlying};
use crate::policy::is_linux_system_mount;
use crate::raw::Geometry;
use crate::{OpenOptions, Privilege};

const SYS: &str = "/sys";
const BLKSSZGET: u32 = 0x1268;
const BLKPBSZGET: u32 = 0x127b;
const BLKGETSIZE64: u32 = 0x8008_1272;
const BLKRRPART: u32 = 0x125f;

pub(crate) struct Hold {
    device: String,
    helper: bool,
}

pub(crate) fn list() -> Result<Vec<Disk>> {
    let sys = Path::new(SYS);
    let devices = read_block_devices(sys).map_err(|e| Error::io("read /sys/block", e))?;
    let mounts = parse_mountinfo(&read("/proc/self/mountinfo")?);
    let mut system: HashSet<(u32, u32)> = HashSet::new();
    for m in mounts.iter().filter(|m| is_linux_system_mount(&m.mount_point)) {
        system.extend(devices_under(sys, m));
    }
    for swap in parse_swaps(&fs::read_to_string("/proc/swaps").unwrap_or_default()) {
        if let Some(device) = block_device_of(&swap) {
            system.extend(underlying(sys, device));
        }
    }
    Ok(devices.iter().map(|d| disk(sys, d, &mounts, &system)).collect())
}

fn disk(sys: &Path, d: &BlockDevice, mounts: &[Mount], system: &HashSet<(u32, u32)>) -> Disk {
    let volumes_on = |device: (u32, u32)| -> Vec<Volume> {
        mounts
            .iter()
            .filter(|m| devices_under(sys, m).contains(&device))
            .map(|m| Volume {
                mount_point: m.mount_point.clone(),
                file_system: m.file_system.clone(),
            })
            .collect()
    };
    let partitions = d
        .partitions
        .iter()
        .map(|p| Partition {
            path: dev_path(&p.name),
            number: p.number,
            offset: p.offset,
            size: p.size,
            name: p.label.clone(),
            kind: String::new(),
            volumes: volumes_on(p.device),
        })
        .collect();
    let on_disk = std::iter::once(d.device).chain(d.partitions.iter().map(|p| p.device));
    let seq = d
        .diskseq
        .map_or_else(|| format!("{}:{}", d.device.0, d.device.1), |s| s.to_string());
    Disk {
        id: format!("linux:{}:{seq}", d.name),
        path: dev_path(&d.name),
        size: d.size,
        logical_sector: d.logical,
        physical_sector: d.physical,
        removable: d.removable,
        internal: !d.removable && !matches!(d.bus, Bus::Usb | Bus::Sd | Bus::Virtual),
        system: on_disk.into_iter().any(|dev| system.contains(&dev)),
        read_only: d.read_only,
        model: d.model.clone(),
        bus: d.bus.clone(),
        partitions,
        volumes: volumes_on(d.device),
    }
}

pub(crate) fn open(disk: &Disk, options: &OpenOptions) -> Result<(File, Geometry, Hold)> {
    let device = disk.path.to_string_lossy().into_owned();
    // SAFETY: geteuid has no preconditions.
    let helper = options.privilege == Privilege::Helper || unsafe { libc::geteuid() } != 0;
    for volume in disk.mounted() {
        if helper {
            let source = mount_source(disk, &volume.mount_point).unwrap_or_else(|| device.clone());
            udisks::unmount(&source, options.force_unmount)?;
        } else {
            unmount(&volume.mount_point)?;
        }
    }
    let flags = libc::O_DIRECT | libc::O_EXCL | libc::O_CLOEXEC;
    let file = if helper {
        File::from(udisks::open(&device, flags)?)
    } else {
        FsOpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(flags)
            .open(&disk.path)
            .map_err(|e| busy_or_io(&disk.path, e))?
    };
    let geometry = geometry(&file).map_err(|e| Error::io(format!("geometry of {device}"), e))?;
    Ok((file, geometry, Hold { device, helper }))
}

impl Hold {
    pub(crate) fn release(self, file: File) -> Result<()> {
        let flushed = flush(&file);
        drop(file);
        self.reread()?;
        flushed.map_err(|e| Error::io(format!("flush {}", self.device), e))
    }

    pub(crate) fn eject(self, file: File) -> Result<()> {
        flush(&file).map_err(|e| Error::io(format!("flush {}", self.device), e))?;
        drop(file);
        self.reread()?;
        udisks::eject(&self.device)
    }

    fn reread(&self) -> Result<()> {
        if self.helper {
            return udisks::rescan(&self.device);
        }
        let file = File::open(&self.device).map_err(|e| Error::io(format!("open {}", self.device), e))?;
        ioctl(&file, BLKRRPART, &mut 0i32)
            .map_err(|e| Error::io(format!("re-read the partitions of {}", self.device), e))
    }
}

/// fsync on a block device sends the drive a cache flush.
pub(crate) fn flush(file: &File) -> io::Result<()> {
    file.sync_all()
}

fn geometry(file: &File) -> io::Result<Geometry> {
    let mut logical: libc::c_int = 0;
    let mut physical: libc::c_uint = 0;
    let mut size: u64 = 0;
    ioctl(file, BLKSSZGET, &mut logical)?;
    ioctl(file, BLKPBSZGET, &mut physical)?;
    ioctl(file, BLKGETSIZE64, &mut size)?;
    Ok(Geometry {
        size,
        logical: logical as u32,
        physical: physical.max(logical as u32),
    })
}

fn ioctl<T>(file: &File, request: u32, value: &mut T) -> io::Result<()> {
    // SAFETY: each request reads or writes one T, as linux/fs.h declares it.
    if unsafe { libc::ioctl(file.as_raw_fd(), request as _, value as *mut T) } == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn unmount(mount_point: &Path) -> Result<()> {
    let path = CString::new(mount_point.as_os_str().as_bytes())
        .map_err(|_| Error::Changed(mount_point.display().to_string()))?;
    // SAFETY: a NUL-terminated path.
    if unsafe { libc::umount2(path.as_ptr(), 0) } == 0 {
        return Ok(());
    }
    Err(busy_or_io(mount_point, io::Error::last_os_error()))
}

fn busy_or_io(path: &Path, e: io::Error) -> Error {
    if e.raw_os_error() == Some(libc::EBUSY) {
        Error::Busy {
            volume: path.display().to_string(),
            detail: "in use, or held by another device (dm-crypt, md, LVM)".into(),
        }
    } else {
        Error::io(format!("open {}", path.display()), e)
    }
}

/// The partition device a mount point's volume is on, for udisks, which unmounts by device.
fn mount_source(disk: &Disk, mount_point: &Path) -> Option<String> {
    disk.partitions
        .iter()
        .find(|p| p.volumes.iter().any(|v| v.mount_point == mount_point))
        .map(|p| p.path.to_string_lossy().into_owned())
}

/// btrfs and other multi-device file systems report an anonymous `0:N` device in mountinfo, so their source path
/// names the block device instead.
fn devices_under(sys: &Path, mount: &Mount) -> Vec<(u32, u32)> {
    let device = match mount.device {
        (0, _) => block_device_of(Path::new(&mount.source)),
        device => Some(device),
    };
    device.map(|d| underlying(sys, d)).unwrap_or_default()
}

fn block_device_of(path: &Path) -> Option<(u32, u32)> {
    let meta = fs::metadata(path).ok()?;
    if !meta.file_type().is_block_device() {
        return None;
    }
    let rdev = meta.rdev();
    let major = ((rdev >> 8) & 0xfff) | ((rdev >> 32) & !0xfff);
    let minor = (rdev & 0xff) | ((rdev >> 12) & !0xff);
    Some((major as u32, minor as u32))
}

fn read(path: &str) -> Result<String> {
    fs::read_to_string(path).map_err(|e| Error::io(format!("read {path}"), e))
}
