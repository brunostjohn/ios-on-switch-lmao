// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::ffi::{CString, c_void};
use std::path::PathBuf;
use std::ptr::NonNull;

use objc2_core_foundation::{CFBoolean, CFDictionary, CFString, CFType};
use objc2_disk_arbitration::{
    DADisk, DASession, kDADiskDescriptionDeviceInternalKey, kDADiskDescriptionDeviceModelKey,
    kDADiskDescriptionDeviceProtocolKey, kDADiskDescriptionDeviceVendorKey, kDADiskDescriptionMediaEjectableKey,
    kDADiskDescriptionMediaRemovableKey,
};

use super::iokit::{self, Media};
use super::mounts::{self, Mount};
use crate::error::{Error, Result};
use crate::model::{Bus, Disk, Partition, Volume};
use crate::policy::is_macos_system_mount;

pub fn list() -> Result<Vec<Disk>> {
    let media = iokit::all_media()?;
    let mounts = mounts::mounts().map_err(|e| Error::io("list mounted volumes", e))?;
    // SAFETY: DASessionCreate with the default allocator; descriptions need no run loop.
    let session = unsafe { DASession::new(None) }.ok_or_else(|| Error::os("DiskArbitration", "no session"))?;
    let by_bsd: HashMap<&str, &Media> = media.iter().map(|m| (m.bsd.as_str(), m)).collect();
    let mut disks: Vec<Disk> = media
        .iter()
        .filter(|m| m.ancestors.is_empty())
        .map(|root| disk(&session, root, &media, &mounts, &by_bsd))
        .collect();
    disks.sort_by_key(|d| unit(&d.path));
    Ok(disks)
}

pub fn id_of(media: &Media) -> String {
    format!("macos:{:x}", media.id)
}

pub fn parse_id(id: &str) -> Option<u64> {
    u64::from_str_radix(id.strip_prefix("macos:")?, 16).ok()
}

fn disk(session: &DASession, root: &Media, all: &[Media], mounts: &[Mount], by_bsd: &HashMap<&str, &Media>) -> Disk {
    let info = Description::of(session, &root.bsd);
    let mut partitions: Vec<Partition> = all
        .iter()
        .filter(|m| m.ancestors.first() == Some(&root.id))
        .map(|m| Partition {
            path: PathBuf::from(format!("/dev/{}", m.bsd)),
            number: m.number,
            offset: m.base,
            size: m.size,
            name: m.name.clone(),
            kind: m.content.clone(),
            volumes: Vec::new(),
        })
        .collect();
    partitions.sort_by_key(|p| p.offset);
    let mut volumes = Vec::new();
    let mut system = false;
    for mount in mounts {
        let Some(media) = by_bsd.get(mount.bsd.as_str()) else {
            continue;
        };
        if !media.is_on(root.id) {
            continue;
        }
        system |= is_macos_system_mount(&mount.mount_point);
        let volume = Volume {
            mount_point: mount.mount_point.clone(),
            file_system: mount.file_system.clone(),
        };
        let partition = partition_of(media, root.id, all);
        match partition.and_then(|bsd| partitions.iter_mut().find(|p| p.path.ends_with(bsd))) {
            Some(partition) => partition.volumes.push(volume),
            None => volumes.push(volume),
        }
    }
    Disk {
        id: id_of(root),
        path: PathBuf::from(format!("/dev/{}", root.bsd)),
        size: root.size,
        logical_sector: root.block,
        physical_sector: root.block,
        removable: info.removable,
        internal: info.internal,
        system,
        read_only: !root.writable,
        model: info.model,
        bus: info.bus,
        partitions,
        volumes,
    }
}

/// The BSD name of the partition of `root` that `media` sits on: itself, or for an APFS volume the partition
/// holding its container.
fn partition_of<'a>(media: &'a Media, root: u64, all: &'a [Media]) -> Option<&'a str> {
    if media.ancestors.first() == Some(&root) {
        return Some(&media.bsd);
    }
    let index = media.ancestors.iter().position(|&a| a == root)?;
    let partition = *media.ancestors.get(index.checked_sub(1)?)?;
    all.iter().find(|m| m.id == partition).map(|m| m.bsd.as_str())
}

fn unit(path: &std::path::Path) -> u64 {
    path.to_string_lossy()
        .trim_start_matches("/dev/disk")
        .parse()
        .unwrap_or(u64::MAX)
}

struct Description {
    internal: bool,
    removable: bool,
    model: String,
    bus: Bus,
}

impl Description {
    fn of(session: &DASession, bsd: &str) -> Description {
        let description = CString::new(bsd).ok().and_then(|name| {
            let ptr = NonNull::new(name.as_ptr().cast_mut())?;
            // SAFETY: a NUL-terminated BSD name that lives across the call.
            let disk = unsafe { DADisk::from_bsd_name(None, session, ptr) }?;
            unsafe { disk.description() }
        });
        let Some(d) = description else {
            return Description {
                internal: true,
                removable: false,
                model: String::new(),
                bus: Bus::Other(String::new()),
            };
        };
        // SAFETY: DiskArbitration's description keys are static CFStrings.
        let (internal, removable, ejectable, protocol, vendor, model) = unsafe {
            (
                flag(&d, kDADiskDescriptionDeviceInternalKey),
                flag(&d, kDADiskDescriptionMediaRemovableKey),
                flag(&d, kDADiskDescriptionMediaEjectableKey),
                text(&d, kDADiskDescriptionDeviceProtocolKey),
                text(&d, kDADiskDescriptionDeviceVendorKey),
                text(&d, kDADiskDescriptionDeviceModelKey),
            )
        };
        Description {
            internal,
            removable: removable || ejectable,
            model: [vendor, model]
                .into_iter()
                .filter(|s| !s.is_empty())
                .collect::<Vec<_>>()
                .join(" "),
            bus: bus(&protocol),
        }
    }
}

fn bus(protocol: &str) -> Bus {
    match protocol {
        "USB" => Bus::Usb,
        "Secure Digital" | "SD" => Bus::Sd,
        "SATA" | "ATA" => Bus::Sata,
        "PCI-Express" | "PCI" | "NVMe" | "Apple Fabric" => Bus::Nvme,
        "SAS" | "SCSI" | "Thunderbolt" => Bus::Scsi,
        "Disk Image" | "Virtual Interface" => Bus::Virtual,
        other => Bus::Other(other.to_owned()),
    }
}

fn value<'a>(dictionary: &'a CFDictionary, key: &CFString) -> Option<&'a CFType> {
    // SAFETY: the dictionary holds CF objects; a present value lives as long as the dictionary.
    unsafe { (dictionary.value((key as *const CFString).cast::<c_void>()) as *const CFType).as_ref() }
}

fn flag(dictionary: &CFDictionary, key: &CFString) -> bool {
    value(dictionary, key)
        .and_then(|v| v.downcast_ref::<CFBoolean>())
        .is_some_and(|b| b.as_bool())
}

fn text(dictionary: &CFDictionary, key: &CFString) -> String {
    value(dictionary, key)
        .and_then(|v| v.downcast_ref::<CFString>())
        .map(|s| s.to_string().trim().to_owned())
        .unwrap_or_default()
}
