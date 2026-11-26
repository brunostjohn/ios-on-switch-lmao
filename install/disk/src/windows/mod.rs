// SPDX-License-Identifier: GPL-3.0-or-later
//! Every volume on the disk is locked and dismounted, and its handle kept, so Windows mounts none of them until
//! release unlocks them. Windows notices a changed partition table only when told (IOCTL_DISK_UPDATE_PROPERTIES),
//! which release and eject do after the last write.
mod ffi;
mod list;

use std::fs::{File, OpenOptions as FsOpenOptions};
use std::io;
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle};

use windows_sys::Win32::Foundation::HANDLE;
use windows_sys::Win32::Security::{GetTokenInformation, TOKEN_ELEVATION, TOKEN_QUERY, TokenElevation};
use windows_sys::Win32::Storage::FileSystem::{
    FILE_FLAG_NO_BUFFERING, FILE_FLAG_WRITE_THROUGH, FILE_SHARE_READ, FILE_SHARE_WRITE, FlushFileBuffers,
};
use windows_sys::Win32::System::Ioctl::{
    FSCTL_DISMOUNT_VOLUME, FSCTL_LOCK_VOLUME, FSCTL_UNLOCK_VOLUME, IOCTL_DISK_GET_LENGTH_INFO,
    IOCTL_DISK_UPDATE_PROPERTIES, IOCTL_STORAGE_EJECT_MEDIA,
};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

pub(crate) use list::list;

use crate::OpenOptions;
use crate::error::{Error, Result};
use crate::model::Disk;
use crate::raw::Geometry;

pub(crate) struct Hold {
    path: String,
    volumes: Vec<OwnedHandle>,
}

pub(crate) fn open(disk: &Disk, options: &OpenOptions) -> Result<(File, Geometry, Hold)> {
    if !elevated() {
        return Err(Error::NotElevated);
    }
    let path = disk.path.to_string_lossy().into_owned();
    let number: u32 = path
        .trim_start_matches(r"\\.\PhysicalDrive")
        .parse()
        .map_err(|_| Error::Changed(path.clone()))?;
    let mut hold = Hold {
        path: path.clone(),
        volumes: Vec::new(),
    };
    for volume in list::volume_infos()
        .into_iter()
        .filter(|v| v.extents.iter().any(|e| e.disk == number))
    {
        hold.volumes.push(lock(&volume.device, options.force_unmount)?);
    }
    let file = FsOpenOptions::new()
        .read(true)
        .write(true)
        .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
        .custom_flags(FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)
        .open(&path)
        .map_err(|e| Error::io(format!("open {path}"), e))?;
    let mut length = [0u8; 8];
    ffi::ioctl(&file, IOCTL_DISK_GET_LENGTH_INFO, &[], &mut length)
        .map_err(|e| Error::io(format!("size of {path}"), e))?;
    let geometry = Geometry {
        size: u64::from_le_bytes(length),
        logical: disk.logical_sector,
        physical: disk.physical_sector,
    };
    Ok((file, geometry, hold))
}

/// Locks a volume for this process alone and dismounts it. With `force`, the dismount comes first, which cuts off
/// the programs that have files open on it.
fn lock(device: &str, force: bool) -> Result<OwnedHandle> {
    let handle = ffi::open(device, true).map_err(|e| Error::io(format!("open {device}"), e))?;
    if force {
        ffi::ioctl(&handle, FSCTL_DISMOUNT_VOLUME, &[], &mut [])
            .map_err(|e| Error::io(format!("dismount {device}"), e))?;
    }
    ffi::ioctl(&handle, FSCTL_LOCK_VOLUME, &[], &mut []).map_err(|_| Error::Busy {
        volume: device.to_owned(),
        detail: "a program has files open on it".into(),
    })?;
    ffi::ioctl(&handle, FSCTL_DISMOUNT_VOLUME, &[], &mut []).map_err(|e| Error::io(format!("dismount {device}"), e))?;
    Ok(handle)
}

impl Hold {
    pub(crate) fn release(self, file: File) -> Result<()> {
        let flushed = flush(&file);
        let updated = ffi::ioctl(&file, IOCTL_DISK_UPDATE_PROPERTIES, &[], &mut []);
        drop(file);
        self.unlock();
        flushed.map_err(|e| Error::io(format!("flush {}", self.path), e))?;
        updated
            .map(|_| ())
            .map_err(|e| Error::io(format!("re-read the partitions of {}", self.path), e))
    }

    pub(crate) fn eject(self, file: File) -> Result<()> {
        flush(&file).map_err(|e| Error::io(format!("flush {}", self.path), e))?;
        ffi::ioctl(&file, IOCTL_DISK_UPDATE_PROPERTIES, &[], &mut [])
            .map_err(|e| Error::io(format!("re-read the partitions of {}", self.path), e))?;
        let ejected = ffi::ioctl(&file, IOCTL_STORAGE_EJECT_MEDIA, &[], &mut []);
        drop(file);
        self.unlock();
        ejected
            .map(|_| ())
            .map_err(|e| Error::io(format!("eject {}", self.path), e))
    }

    fn unlock(&self) {
        for volume in &self.volumes {
            let _ = ffi::ioctl(volume, FSCTL_UNLOCK_VOLUME, &[], &mut []);
        }
    }
}

pub(crate) fn flush(file: &File) -> io::Result<()> {
    // SAFETY: a live handle we own.
    if unsafe { FlushFileBuffers(file.as_raw_handle() as HANDLE) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn elevated() -> bool {
    let mut token: HANDLE = std::ptr::null_mut();
    // SAFETY: the current process pseudo-handle needs no closing; the token handle is owned below.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return false;
    }
    // SAFETY: OpenProcessToken returned a handle we now own.
    let token = unsafe { OwnedHandle::from_raw_handle(token as _) };
    let mut elevation = TOKEN_ELEVATION { TokenIsElevated: 0 };
    let mut returned = 0u32;
    // SAFETY: the buffer is one TOKEN_ELEVATION and its size is passed.
    let ok = unsafe {
        GetTokenInformation(
            token.as_raw_handle() as HANDLE,
            TokenElevation,
            (&mut elevation as *mut TOKEN_ELEVATION).cast(),
            size_of::<TOKEN_ELEVATION>() as u32,
            &mut returned,
        )
    };
    ok != 0 && elevation.TokenIsElevated != 0
}
