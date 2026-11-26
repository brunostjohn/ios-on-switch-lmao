// SPDX-License-Identifier: GPL-3.0-or-later
//! File bytes that start 16-byte aligned: the translator reads them through word pointers.

use std::ffi::c_void;
use std::io;
use std::ops::{Deref, DerefMut};
use std::ptr::{self, NonNull};

const ALIGN: usize = 16;

pub enum Buffer {
    Vec(Vec<u8>),
    Words { words: Vec<u128>, len: usize },
}

impl Buffer {
    pub fn zeroed(len: usize) -> Self {
        Buffer::Words {
            words: vec![0u128; len.div_ceil(ALIGN)],
            len,
        }
    }

    /// `bytes` itself when its allocation is aligned, which the system allocator gives any block of 16 bytes or more;
    /// otherwise a copy.
    pub fn from_vec(bytes: Vec<u8>) -> Self {
        if (bytes.as_ptr() as usize).is_multiple_of(ALIGN) {
            Buffer::Vec(bytes)
        } else {
            let mut out = Buffer::zeroed(bytes.len());
            out.copy_from_slice(&bytes);
            out
        }
    }
}

impl Deref for Buffer {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Buffer::Vec(v) => v,
            // SAFETY: `words` holds at least `len` initialised bytes, and u128 has no padding.
            Buffer::Words { words, len } => unsafe { std::slice::from_raw_parts(words.as_ptr().cast(), *len) },
        }
    }
}

impl DerefMut for Buffer {
    fn deref_mut(&mut self) -> &mut [u8] {
        match self {
            Buffer::Vec(v) => v,
            // SAFETY: as in `deref`, and the borrow is unique.
            Buffer::Words { words, len } => unsafe { std::slice::from_raw_parts_mut(words.as_mut_ptr().cast(), *len) },
        }
    }
}

/// Anonymous zero-filled OS pages (`mmap(MAP_ANON)` / `VirtualAlloc`) returned directly to the OS on drop so large
/// transient buffers never linger in `libc` malloc caches across worker threads.
pub struct AnonBuffer {
    ptr: *mut u8,
    len: usize,
}

// SAFETY: `AnonBuffer` uniquely owns its anonymous mapping.
unsafe impl Send for AnonBuffer {}
unsafe impl Sync for AnonBuffer {}

impl AnonBuffer {
    pub fn zeroed(len: usize) -> io::Result<Self> {
        if len == 0 {
            return Ok(Self {
                ptr: NonNull::<u128>::dangling().as_ptr().cast::<u8>(),
                len: 0,
            });
        }
        let ptr = alloc_anon(len)?;
        Ok(Self { ptr, len })
    }
}

impl Deref for AnonBuffer {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        // SAFETY: `ptr..ptr + len` is a live zero-initialized mapping (or dangling with `len == 0`).
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }
}

impl DerefMut for AnonBuffer {
    fn deref_mut(&mut self) -> &mut [u8] {
        // SAFETY: `self` uniquely owns `ptr..ptr + len`.
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
    }
}

impl Drop for AnonBuffer {
    fn drop(&mut self) {
        if self.len > 0 {
            free_anon(self.ptr, self.len);
        }
    }
}

#[cfg(unix)]
fn alloc_anon(len: usize) -> io::Result<*mut u8> {
    // SAFETY: `len > 0`; anonymous private mapping has no file descriptor precondition.
    let p = unsafe {
        libc::mmap(
            ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANON,
            -1,
            0,
        )
    };
    if p == libc::MAP_FAILED {
        Err(io::Error::last_os_error())
    } else {
        Ok(p.cast::<u8>())
    }
}

#[cfg(unix)]
fn free_anon(ptr: *mut u8, len: usize) {
    // SAFETY: `ptr` and `len` came from `alloc_anon` and are unmapped once.
    unsafe {
        libc::munmap(ptr.cast::<c_void>(), len);
    }
}

#[cfg(windows)]
fn alloc_anon(len: usize) -> io::Result<*mut u8> {
    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const PAGE_READWRITE: u32 = 0x04;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn VirtualAlloc(addr: *mut c_void, size: usize, alloc_type: u32, protect: u32) -> *mut c_void;
    }

    // SAFETY: `len > 0`; `VirtualAlloc` returns null on failure.
    let p = unsafe { VirtualAlloc(ptr::null_mut(), len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if p.is_null() {
        Err(io::Error::last_os_error())
    } else {
        Ok(p.cast::<u8>())
    }
}

#[cfg(windows)]
fn free_anon(ptr: *mut u8, _len: usize) {
    const MEM_RELEASE: u32 = 0x8000;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn VirtualFree(addr: *mut c_void, size: usize, free_type: u32) -> i32;
    }

    // SAFETY: `ptr` came from `alloc_anon` and is released once with `size == 0` as `MEM_RELEASE` requires.
    unsafe {
        VirtualFree(ptr.cast::<c_void>(), 0, MEM_RELEASE);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn anon_buffer_empty_and_multipage() {
        let empty = AnonBuffer::zeroed(0).unwrap();
        assert!(empty.is_empty());
        assert_eq!(empty.as_ptr() as usize % 16, 0);

        let mut buf = AnonBuffer::zeroed(140_000).unwrap();
        assert_eq!(buf.as_ptr() as usize % 16, 0);
        assert!(buf.iter().all(|&b| b == 0));
        buf[0] = 0xaa;
        buf[65_536] = 0x55;
        buf[139_999] = 0xff;
        assert_eq!((buf[0], buf[65_536], buf[139_999]), (0xaa, 0x55, 0xff));
    }
}
