// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{OsStr, OsString, c_void};
use std::io;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle};
use std::ptr::null_mut;

use windows_sys::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, FindFirstVolumeW, FindNextVolumeW, FindVolumeClose,
    GetVolumePathNamesForVolumeNameW, OPEN_EXISTING,
};
use windows_sys::Win32::System::IO::DeviceIoControl;

pub fn wide(text: impl AsRef<OsStr>) -> Vec<u16> {
    text.as_ref().encode_wide().chain(Some(0)).collect()
}

pub fn from_wide(units: &[u16]) -> String {
    let end = units.iter().position(|&u| u == 0).unwrap_or(units.len());
    OsString::from_wide(&units[..end]).to_string_lossy().into_owned()
}

/// A NUL-separated, double-NUL-terminated list.
pub fn multi_sz(units: &[u16]) -> Vec<String> {
    units
        .split(|&u| u == 0)
        .take_while(|s| !s.is_empty())
        .map(from_wide)
        .collect()
}

/// Opens a device for queries (no access) or for reading and writing, sharing it with other openers.
pub fn open(path: &str, write: bool) -> io::Result<OwnedHandle> {
    let access = if write { GENERIC_READ | GENERIC_WRITE } else { 0 };
    let path = wide(path);
    // SAFETY: a NUL-terminated path and no security attributes or template.
    let handle = unsafe {
        CreateFileW(
            path.as_ptr(),
            access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            null_mut(),
            OPEN_EXISTING,
            0,
            null_mut(),
        )
    };
    if handle == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: a handle CreateFileW just returned, owned from here on.
    Ok(unsafe { OwnedHandle::from_raw_handle(handle as RawHandle) })
}

pub fn ioctl(handle: &impl AsRawHandle, code: u32, input: &[u8], output: &mut [u8]) -> io::Result<usize> {
    let mut returned = 0u32;
    let input_ptr = if input.is_empty() {
        null_mut()
    } else {
        input.as_ptr() as *mut c_void
    };
    let output_ptr = if output.is_empty() {
        null_mut()
    } else {
        output.as_mut_ptr().cast()
    };
    // SAFETY: both buffers are valid for their lengths, and the call is synchronous.
    let ok = unsafe {
        DeviceIoControl(
            handle.as_raw_handle().cast(),
            code,
            input_ptr,
            input.len() as u32,
            output_ptr,
            output.len() as u32,
            &mut returned,
            null_mut(),
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(returned as usize)
}

/// Every volume's GUID path, `\\?\Volume{...}\`.
pub fn volumes() -> Vec<String> {
    let mut name = [0u16; 64];
    // SAFETY: the buffer's length is passed.
    let find = unsafe { FindFirstVolumeW(name.as_mut_ptr(), name.len() as u32) };
    if find == INVALID_HANDLE_VALUE {
        return Vec::new();
    }
    let mut found = vec![from_wide(&name)];
    // SAFETY: as above, on the search handle FindFirstVolumeW returned.
    while unsafe { FindNextVolumeW(find, name.as_mut_ptr(), name.len() as u32) } != 0 {
        found.push(from_wide(&name));
    }
    // SAFETY: closes the search handle once.
    unsafe { FindVolumeClose(find) };
    found
}

/// Where a volume is mounted: drive letters and folders.
pub fn mount_points(volume: &str) -> Vec<String> {
    let name = wide(volume);
    let mut paths = vec![0u16; 1024];
    let mut needed = 0u32;
    // SAFETY: the buffer's length is passed; Windows fills a multi-string within it.
    let ok =
        unsafe { GetVolumePathNamesForVolumeNameW(name.as_ptr(), paths.as_mut_ptr(), paths.len() as u32, &mut needed) };
    if ok == 0 { Vec::new() } else { multi_sz(&paths) }
}
