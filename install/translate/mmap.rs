// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only file-backed memory mappings (`mmap` on Unix, `CreateFileMappingW`/`MapViewOfFile` on Windows), with
//! sub-range alignment and clean handling of empty files.

use std::ffi::c_void;
use std::fs::{self, File};
use std::io;
use std::ops::Deref;
use std::path::{Path, PathBuf};
use std::ptr;

/// 16-byte aligned sentinel for empty mappings so C callers requiring alignment never see a 1-byte dangling pointer.
static EMPTY: [u128; 1] = [0];

pub struct ReadOnlyMap {
    base: *mut c_void,
    map_len: usize,
    data: *const u8,
    len: usize,
}

// SAFETY: `ReadOnlyMap` exposes only shared immutable `&[u8]` views over a private read-only OS mapping whose
// lifetime is tied to `self`.
unsafe impl Send for ReadOnlyMap {}
unsafe impl Sync for ReadOnlyMap {}

impl ReadOnlyMap {
    pub fn empty() -> Self {
        Self {
            base: ptr::null_mut(),
            map_len: 0,
            data: EMPTY.as_ptr().cast::<u8>(),
            len: 0,
        }
    }

    pub fn open(path: &Path) -> io::Result<Self> {
        let file = File::open(path)?;
        Self::file(&file)
    }

    pub fn file(file: &File) -> io::Result<Self> {
        let file_len = file.metadata()?.len();
        let len = usize::try_from(file_len).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("file length {file_len} exceeds usize"),
            )
        })?;
        Self::range(file, 0, len)
    }

    pub fn range(file: &File, offset: u64, len: usize) -> io::Result<Self> {
        let file_len = file.metadata()?.len();
        let end = offset
            .checked_add(len as u64)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "mapping range overflows u64"))?;
        if end > file_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("mapping range {offset}..{end} exceeds file length {file_len}"),
            ));
        }
        if len == 0 {
            return Ok(Self::empty());
        }
        map_range(file, offset, len)
    }

    #[cfg(unix)]
    fn advise(&self, advice: libc::c_int) {
        if self.map_len > 0 {
            // SAFETY: `base..base + map_len` is a live read-only file-backed mapping owned by `self`.
            unsafe {
                libc::madvise(self.base, self.map_len, advice);
            }
        }
    }

    pub fn advise_sequential(&self) {
        #[cfg(unix)]
        self.advise(libc::MADV_SEQUENTIAL);
    }

    pub fn advise_normal(&self) {
        #[cfg(unix)]
        self.advise(libc::MADV_NORMAL);
    }

    pub fn advise_dontneed(&self) {
        #[cfg(unix)]
        self.advise(libc::MADV_DONTNEED);
    }
}

impl Deref for ReadOnlyMap {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        // SAFETY: `data..data + len` lies within the live read-only mapping (or `EMPTY` when `len == 0`).
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }
}

impl AsRef<[u8]> for ReadOnlyMap {
    fn as_ref(&self) -> &[u8] {
        self
    }
}

impl Drop for ReadOnlyMap {
    fn drop(&mut self) {
        if self.map_len > 0 {
            unmap_raw(self.base, self.map_len);
        }
    }
}

/// A read-only mapping backed either directly by an existing file or by a temporary file deleted after unmapping.
pub enum Mapped {
    Direct(ReadOnlyMap),
    Temp {
        /// Declared before `_file` so the mapping is unmapped before deletion (required on Windows).
        map: ReadOnlyMap,
        _file: TempFile,
    },
}

pub struct TempFile(PathBuf);

impl TempFile {
    pub fn new(path: PathBuf) -> Self {
        Self(path)
    }

    pub fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

impl Mapped {
    pub fn temp(map: ReadOnlyMap, file: TempFile) -> Self {
        Mapped::Temp { map, _file: file }
    }

    fn inner(&self) -> &ReadOnlyMap {
        match self {
            Mapped::Direct(m) | Mapped::Temp { map: m, .. } => m,
        }
    }

    pub fn advise_sequential(&self) {
        self.inner().advise_sequential();
    }

    pub fn advise_normal(&self) {
        self.inner().advise_normal();
    }

    pub fn advise_dontneed(&self) {
        self.inner().advise_dontneed();
    }
}

impl Deref for Mapped {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        self.inner()
    }
}

impl AsRef<[u8]> for Mapped {
    fn as_ref(&self) -> &[u8] {
        self
    }
}

#[cfg(unix)]
fn map_range(file: &File, offset: u64, len: usize) -> io::Result<ReadOnlyMap> {
    use std::os::unix::io::AsRawFd;

    // SAFETY: `sysconf(_SC_PAGESIZE)` has no preconditions.
    let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    let page = u64::try_from(page)
        .ok()
        .filter(|&p| p > 0)
        .ok_or_else(|| io::Error::other("invalid OS page size"))?;
    let rem = offset % page;
    let map_offset = offset - rem;
    let delta = usize::try_from(rem).map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "alignment overflow"))?;
    let map_len = delta
        .checked_add(len)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "mapping length overflow"))?;
    let raw_offset = libc::off_t::try_from(map_offset)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "mapping offset exceeds off_t"))?;

    // SAFETY: `file` is a valid open file descriptor, `map_len > 0`, and `raw_offset` is page-aligned.
    let base = unsafe {
        libc::mmap(
            ptr::null_mut(),
            map_len,
            libc::PROT_READ,
            libc::MAP_PRIVATE,
            file.as_raw_fd(),
            raw_offset,
        )
    };
    if base == libc::MAP_FAILED {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: `delta + len == map_len`, so `base.add(delta)` lies inside the mapping.
    let data = unsafe { base.cast::<u8>().add(delta) };
    Ok(ReadOnlyMap {
        base,
        map_len,
        data,
        len,
    })
}

#[cfg(unix)]
fn unmap_raw(base: *mut c_void, map_len: usize) {
    // SAFETY: `base` and `map_len` came from `libc::mmap` in `map_range` and are unmapped exactly once.
    unsafe {
        libc::munmap(base, map_len);
    }
}

#[cfg(windows)]
fn map_range(file: &File, offset: u64, len: usize) -> io::Result<ReadOnlyMap> {
    use std::mem::MaybeUninit;
    use std::os::windows::io::AsRawHandle;

    use windows_sys::Win32::Foundation::CloseHandle;
    use windows_sys::Win32::System::SystemInformation::{GetSystemInfo, SYSTEM_INFO};

    const PAGE_READONLY: u32 = 0x02;
    const FILE_MAP_READ: u32 = 0x0004;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn CreateFileMappingW(
            file: *mut c_void,
            attrs: *const c_void,
            protect: u32,
            max_size_high: u32,
            max_size_low: u32,
            name: *const u16,
        ) -> *mut c_void;
        fn MapViewOfFile(
            mapping: *mut c_void,
            access: u32,
            offset_high: u32,
            offset_low: u32,
            bytes_to_map: usize,
        ) -> *mut c_void;
    }

    let mut info = MaybeUninit::<SYSTEM_INFO>::zeroed();
    // SAFETY: `GetSystemInfo` initializes `info`.
    let gran = unsafe {
        GetSystemInfo(info.as_mut_ptr());
        u64::from(info.assume_init().dwAllocationGranularity)
    };
    if gran == 0 {
        return Err(io::Error::other("invalid OS allocation granularity"));
    }
    let rem = offset % gran;
    let map_offset = offset - rem;
    let delta = usize::try_from(rem).map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "alignment overflow"))?;
    let map_len = delta
        .checked_add(len)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "mapping length overflow"))?;

    // SAFETY: `file` is a valid non-empty file handle; `CreateFileMappingW` returns NULL on error.
    let mapping = unsafe { CreateFileMappingW(file.as_raw_handle(), ptr::null(), PAGE_READONLY, 0, 0, ptr::null()) };
    if mapping.is_null() {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: `mapping` is a valid read-only file mapping and `map_offset` is aligned to `dwAllocationGranularity`.
    let base = unsafe {
        MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            (map_offset >> 32) as u32,
            map_offset as u32,
            map_len,
        )
    };
    let err = if base.is_null() {
        Some(io::Error::last_os_error())
    } else {
        None
    };
    // SAFETY: Mapped views hold their own reference to the section object, so closing the mapping handle is safe.
    unsafe {
        CloseHandle(mapping);
    }
    if let Some(e) = err {
        return Err(e);
    }
    // SAFETY: `delta + len == map_len`, so `base.add(delta)` lies inside the mapped view.
    let data = unsafe { base.cast::<u8>().add(delta) };
    Ok(ReadOnlyMap {
        base,
        map_len,
        data,
        len,
    })
}

#[cfg(windows)]
fn unmap_raw(base: *mut c_void, _map_len: usize) {
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn UnmapViewOfFile(base: *const c_void) -> i32;
    }
    // SAFETY: `base` came from `MapViewOfFile` in `map_range` and is unmapped exactly once.
    unsafe {
        UnmapViewOfFile(base);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_path(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("switch_translate_mmap.{}.{name}", std::process::id()))
    }

    #[test]
    fn maps_empty_small_and_multipage_files() {
        let empty = temp_path("empty");
        fs::write(&empty, b"").unwrap();
        let m = ReadOnlyMap::open(&empty).unwrap();
        assert!(m.is_empty());
        assert_eq!(m.as_ptr() as usize % 16, 0);
        drop(m);
        fs::remove_file(&empty).unwrap();

        let small = temp_path("small");
        fs::write(&small, b"hello switch").unwrap();
        let m = ReadOnlyMap::open(&small).unwrap();
        assert_eq!(&m[..], b"hello switch");
        m.advise_sequential();
        m.advise_normal();
        m.advise_dontneed();
        drop(m);
        fs::remove_file(&small).unwrap();

        let big = temp_path("multipage");
        let payload: Vec<u8> = (0..150_000usize).map(|i| (i.wrapping_mul(31) & 0xff) as u8).collect();
        fs::write(&big, &payload).unwrap();
        let f = File::open(&big).unwrap();
        let full = ReadOnlyMap::file(&f).unwrap();
        assert_eq!(&full[..], &payload[..]);

        // Unaligned and cross-granularity sub-ranges (covers 4 KiB, 16 KiB and 64 KiB granularities).
        for &(off, len) in &[
            (0u64, 0usize),
            (4096, 8192),
            (16384, 70_000),
            (65_552, 40_000),
            (150_000, 0),
        ] {
            let sub = ReadOnlyMap::range(&f, off, len).unwrap();
            assert_eq!(&sub[..], &payload[off as usize..off as usize + len]);
        }
        drop(full);
        drop(f);
        fs::remove_file(&big).unwrap();
    }

    #[test]
    fn refuses_missing_and_out_of_range() {
        let missing = temp_path("does_not_exist");
        let _ = fs::remove_file(&missing);
        assert!(ReadOnlyMap::open(&missing).is_err());

        let path = temp_path("bounds");
        fs::write(&path, b"0123456789").unwrap();
        let f = File::open(&path).unwrap();
        assert!(ReadOnlyMap::range(&f, 11, 0).is_err());
        assert!(ReadOnlyMap::range(&f, 8, 3).is_err());
        assert!(ReadOnlyMap::range(&f, u64::MAX, 1).is_err());
        drop(f);
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn temp_map_deletes_backing_on_drop() {
        let path = temp_path("temp_drop");
        fs::write(&path, b"ephemeral cache bytes").unwrap();
        let map = ReadOnlyMap::open(&path).unwrap();
        let temp = Mapped::temp(map, TempFile::new(path.clone()));
        assert_eq!(&temp[..], b"ephemeral cache bytes");
        assert!(path.exists());
        drop(temp);
        assert!(!path.exists());
    }
}
