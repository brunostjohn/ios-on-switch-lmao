// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashSet;
use std::mem::{offset_of, size_of};
use std::os::windows::io::OwnedHandle;
use std::path::PathBuf;

use windows_sys::Win32::Foundation::ERROR_WRITE_PROTECT;
use windows_sys::Win32::Storage::FileSystem::{
    GetVolumeInformationW, GetVolumeNameForVolumeMountPointW, GetVolumePathNameW, QueryDosDeviceW,
};
use windows_sys::Win32::System::Ioctl::{
    DISK_EXTENT, DISK_GEOMETRY, DISK_GEOMETRY_EX, DRIVE_LAYOUT_INFORMATION_EX, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
    IOCTL_DISK_GET_DRIVE_LAYOUT_EX, IOCTL_DISK_IS_WRITABLE, IOCTL_STORAGE_QUERY_PROPERTY, PARTITION_INFORMATION_EX,
    PARTITION_INFORMATION_GPT, PropertyStandardQuery, STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, STORAGE_DEVICE_DESCRIPTOR,
    StorageAccessAlignmentProperty, StorageDeviceProperty, VOLUME_DISK_EXTENTS,
};
use windows_sys::Win32::System::SystemInformation::GetSystemWindowsDirectoryW;

use super::ffi::{self, from_wide, wide};
use crate::error::Result;
use crate::model::{Bus, Disk, Partition, Volume};
use crate::parse::storage::{self, Extent};

/// ntddvol.h: CTL_CODE(IOCTL_VOLUME_BASE 'V', 0, METHOD_BUFFERED, FILE_ANY_ACCESS); windows-sys doesn't carry it.
const IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS: u32 = 0x0056_0000;

/// Listing opens disks and volumes with no access rights, which only FILE_ANY_ACCESS control codes accept.
const fn any_access(code: u32) -> bool {
    (code >> 14) & 3 == 0
}

const _: () = {
    assert!(any_access(IOCTL_STORAGE_QUERY_PROPERTY));
    assert!(any_access(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX));
    assert!(any_access(IOCTL_DISK_GET_DRIVE_LAYOUT_EX));
    assert!(any_access(IOCTL_DISK_IS_WRITABLE));
    assert!(any_access(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS));
    assert!(offset_of!(DISK_GEOMETRY_EX, DiskSize) == storage::GEOMETRY_SIZE);
    assert!(offset_of!(DISK_GEOMETRY, BytesPerSector) == storage::GEOMETRY_SECTOR);
    assert!(offset_of!(STORAGE_DEVICE_DESCRIPTOR, RemovableMedia) == storage::DEVICE_DESCRIPTOR_REMOVABLE);
    assert!(offset_of!(STORAGE_DEVICE_DESCRIPTOR, VendorIdOffset) == storage::DEVICE_DESCRIPTOR_VENDOR);
    assert!(offset_of!(STORAGE_DEVICE_DESCRIPTOR, ProductIdOffset) == storage::DEVICE_DESCRIPTOR_PRODUCT);
    assert!(offset_of!(STORAGE_DEVICE_DESCRIPTOR, BusType) == storage::DEVICE_DESCRIPTOR_BUS);
    assert!(offset_of!(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, BytesPerLogicalSector) == storage::ALIGNMENT_LOGICAL);
    assert!(offset_of!(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, BytesPerPhysicalSector) == storage::ALIGNMENT_PHYSICAL);
    assert!(offset_of!(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) == storage::LAYOUT_HEADER);
    assert!(size_of::<PARTITION_INFORMATION_EX>() == storage::LAYOUT_ENTRY);
    assert!(offset_of!(PARTITION_INFORMATION_EX, StartingOffset) == 8);
    assert!(offset_of!(PARTITION_INFORMATION_EX, PartitionLength) == 16);
    assert!(offset_of!(PARTITION_INFORMATION_EX, PartitionNumber) == 24);
    assert!(offset_of!(PARTITION_INFORMATION_EX, Anonymous) == 32);
    assert!(offset_of!(PARTITION_INFORMATION_GPT, Name) == 40);
    assert!(offset_of!(VOLUME_DISK_EXTENTS, Extents) == storage::EXTENTS_HEADER);
    assert!(size_of::<DISK_EXTENT>() == storage::EXTENT);
};

pub struct VolumeInfo {
    /// `\\?\Volume{...}` without the trailing backslash, which CreateFileW opens as the volume itself.
    pub device: String,
    pub extents: Vec<Extent>,
    pub mount_points: Vec<String>,
    pub file_system: String,
}

pub fn list() -> Result<Vec<Disk>> {
    let volumes = volume_infos();
    let system = system_disks(&volumes);
    Ok(physical_drives()
        .into_iter()
        .filter_map(|n| disk(n, &volumes, &system))
        .collect())
}

pub fn volume_infos() -> Vec<VolumeInfo> {
    ffi::volumes()
        .into_iter()
        .filter_map(|name| {
            let device = name.trim_end_matches('\\').to_owned();
            let handle = ffi::open(&device, false).ok()?;
            let mut out = vec![0u8; storage::EXTENTS_HEADER + 32 * storage::EXTENT];
            let n = ffi::ioctl(&handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, &[], &mut out).ok()?;
            Some(VolumeInfo {
                extents: storage::disk_extents(&out[..n])?,
                mount_points: ffi::mount_points(&name),
                file_system: file_system(&name),
                device,
            })
        })
        .collect()
}

fn disk(number: u32, volumes: &[VolumeInfo], system: &HashSet<u32>) -> Option<Disk> {
    let path = format!(r"\\.\PhysicalDrive{number}");
    let handle = ffi::open(&path, false).ok()?;
    let descriptor = query(&handle, StorageDeviceProperty)
        .as_deref()
        .and_then(storage::device_descriptor)?;
    let mut geometry = [0u8; 256];
    let (size, sector) = ffi::ioctl(&handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, &[], &mut geometry)
        .ok()
        .and_then(|n| storage::geometry(&geometry[..n]))
        .unwrap_or((0, 512));
    let (logical, physical) = query(&handle, StorageAccessAlignmentProperty)
        .as_deref()
        .and_then(storage::alignment)
        .unwrap_or((sector, sector));
    let mut layout = vec![0u8; storage::LAYOUT_HEADER + 128 * storage::LAYOUT_ENTRY];
    let layout_len = ffi::ioctl(&handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, &[], &mut layout).unwrap_or(0);
    let layout = &layout[..layout_len];
    let entries = storage::drive_layout(layout).unwrap_or_default();
    let mounted_on = |offset: Option<u64>| -> Vec<Volume> {
        volumes
            .iter()
            .filter(|v| {
                v.extents
                    .iter()
                    .any(|e| e.disk == number && offset.is_none_or(|o| e.offset == o))
            })
            .flat_map(|v| {
                v.mount_points.iter().map(|m| Volume {
                    mount_point: PathBuf::from(m),
                    file_system: v.file_system.clone(),
                })
            })
            .collect()
    };
    let partitions: Vec<Partition> = entries
        .iter()
        .map(|e| Partition {
            path: PathBuf::from(format!(r"\\.\Harddisk{number}Partition{}", e.number)),
            number: e.number,
            offset: e.offset,
            size: e.size,
            name: e.name.clone(),
            kind: e.kind.clone(),
            volumes: mounted_on(Some(e.offset)),
        })
        .collect();
    let read_only = ffi::ioctl(&handle, IOCTL_DISK_IS_WRITABLE, &[], &mut [])
        .is_err_and(|e| e.raw_os_error() == Some(ERROR_WRITE_PROTECT as i32));
    let layout_id = layout.get(8..24).map(storage::guid).unwrap_or_default();
    Some(Disk {
        id: format!("windows:{number}:{layout_id}:{size}"),
        path: PathBuf::from(path),
        size,
        logical_sector: logical,
        physical_sector: physical,
        removable: descriptor.removable,
        internal: !descriptor.removable && !matches!(descriptor.bus, Bus::Usb | Bus::Sd | Bus::Virtual),
        system: system.contains(&number),
        read_only,
        model: descriptor.model,
        bus: descriptor.bus,
        volumes: if partitions.is_empty() {
            mounted_on(None)
        } else {
            Vec::new()
        },
        partitions,
    })
}

fn query(handle: &OwnedHandle, property: i32) -> Option<Vec<u8>> {
    let mut input = [0u8; 12];
    input[..4].copy_from_slice(&property.to_le_bytes());
    input[4..8].copy_from_slice(&PropertyStandardQuery.to_le_bytes());
    let mut out = vec![0u8; 1024];
    let n = ffi::ioctl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &input, &mut out).ok()?;
    out.truncate(n);
    Some(out)
}

/// Every `PhysicalDriveN` in the object manager's DOS device names.
fn physical_drives() -> Vec<u32> {
    let mut names = vec![0u16; 1 << 18];
    // SAFETY: the buffer's length is passed; a null device name asks for every name.
    let n = unsafe { QueryDosDeviceW(std::ptr::null(), names.as_mut_ptr(), names.len() as u32) };
    let mut drives: Vec<u32> = ffi::multi_sz(&names[..n as usize])
        .iter()
        .filter_map(|name| name.strip_prefix("PhysicalDrive")?.parse().ok())
        .collect();
    drives.sort_unstable();
    drives
}

/// The disks holding the volume Windows runs from.
fn system_disks(volumes: &[VolumeInfo]) -> HashSet<u32> {
    let mut directory = [0u16; 260];
    // SAFETY: the buffer's length is passed.
    let n = unsafe { GetSystemWindowsDirectoryW(directory.as_mut_ptr(), directory.len() as u32) } as usize;
    let mut root = [0u16; 260];
    let mut volume = [0u16; 64];
    // SAFETY: NUL-terminated input and buffers whose lengths are passed.
    let found = n > 0
        && unsafe { GetVolumePathNameW(directory.as_ptr(), root.as_mut_ptr(), root.len() as u32) } != 0
        && unsafe { GetVolumeNameForVolumeMountPointW(root.as_ptr(), volume.as_mut_ptr(), volume.len() as u32) } != 0;
    if !found {
        return HashSet::new();
    }
    let device = from_wide(&volume).trim_end_matches('\\').to_owned();
    volumes
        .iter()
        .filter(|v| v.device.eq_ignore_ascii_case(&device))
        .flat_map(|v| v.extents.iter().map(|e| e.disk))
        .collect()
}

fn file_system(volume: &str) -> String {
    let root = wide(volume);
    let mut name = [0u16; 64];
    // SAFETY: a NUL-terminated root path; only the file system name buffer is asked for.
    let ok = unsafe {
        GetVolumeInformationW(
            root.as_ptr(),
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            name.as_mut_ptr(),
            name.len() as u32,
        )
    };
    if ok == 0 { String::new() } else { from_wide(&name) }
}
