// SPDX-License-Identifier: GPL-3.0-or-later
use std::alloc::{Layout, alloc_zeroed, dealloc};
use std::ops::{Deref, DerefMut, Range};
use std::ptr::NonNull;

/// Zeroed heap memory aligned for direct I/O. Writes from one go to the device without a copy.
pub struct AlignedBuf {
    ptr: NonNull<u8>,
    len: usize,
    layout: Layout,
}

// SAFETY: the buffer owns its allocation outright, like a Vec<u8>.
unsafe impl Send for AlignedBuf {}
unsafe impl Sync for AlignedBuf {}

impl AlignedBuf {
    pub fn new(len: usize, align: usize) -> Self {
        let layout = Layout::from_size_align(len.max(1), align).expect("alignment is a power of two");
        // SAFETY: the layout's size is never zero.
        let ptr = unsafe { alloc_zeroed(layout) };
        let ptr = NonNull::new(ptr).unwrap_or_else(|| std::alloc::handle_alloc_error(layout));
        AlignedBuf { ptr, len, layout }
    }
}

impl Deref for AlignedBuf {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        // SAFETY: ptr holds len initialised bytes for as long as self lives.
        unsafe { std::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }
}

impl DerefMut for AlignedBuf {
    fn deref_mut(&mut self) -> &mut [u8] {
        // SAFETY: as in deref, and &mut self is unique.
        unsafe { std::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.len) }
    }
}

impl Drop for AlignedBuf {
    fn drop(&mut self) {
        // SAFETY: allocated in new with this layout.
        unsafe { dealloc(self.ptr.as_ptr(), self.layout) }
    }
}

pub fn align_down(value: u64, to: u64) -> u64 {
    value - value % to
}

pub fn align_up(value: u64, to: u64) -> u64 {
    align_down(value + to - 1, to)
}

/// A device byte range `[offset, offset + len)` and where it sits in the caller's data.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Piece {
    pub offset: u64,
    pub data: Range<usize>,
}

/// An access split into the part that covers whole sectors and the partial sectors at either end, which take a
/// read-modify-write. A range inside one sector is all head.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Split {
    pub head: Option<Piece>,
    pub body: Option<Piece>,
    pub tail: Option<Piece>,
}

pub fn split(offset: u64, len: usize, sector: u32) -> Split {
    let sector = u64::from(sector);
    let end = offset + len as u64;
    let head_end = if offset % sector == 0 {
        offset
    } else {
        end.min(align_up(offset, sector))
    };
    let tail_start = if end % sector == 0 {
        end
    } else {
        head_end.max(align_down(end, sector))
    };
    let piece = |from: u64, to: u64| {
        (from < to).then(|| Piece {
            offset: from,
            data: (from - offset) as usize..(to - offset) as usize,
        })
    };
    Split {
        head: piece(offset, head_end),
        body: piece(head_end, tail_start),
        tail: piece(tail_start, end),
    }
}

pub fn is_aligned(ptr: *const u8, align: usize) -> bool {
    (ptr as usize).is_multiple_of(align)
}
