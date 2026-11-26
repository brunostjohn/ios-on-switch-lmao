// SPDX-License-Identifier: GPL-3.0-or-later
//! File bytes that start 16-byte aligned: the translator reads them through word pointers.

use std::ops::{Deref, DerefMut};

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
