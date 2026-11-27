// SPDX-License-Identifier: GPL-3.0-or-later
//! Only in :switch_translate_perturbed, for refusal_test: a translator whose output differs from the one the release
//! pinned, though every file is still validly signed. In each file's first signed thin slice (a fat file's too), the
//! byte before the signature is flipped and the slice re-signed, so its cdhash, and with it the trust module, changes.

use std::ffi::c_char;

use crate::Error;
use crate::ffi::{CDHASH, ERR_LEN};
use crate::macho::Translated;

const LC_CODE_SIGNATURE: u32 = 0x1d;
const THIN: [u8; 4] = *b"\xcf\xfa\xed\xfe";
const FAT: [u8; 4] = *b"\xca\xfe\xba\xbe";
const FAT64: [u8; 4] = *b"\xca\xfe\xba\xbf";

unsafe extern "C" {
    fn a64x_cs_rehash(
        buf: *mut u8,
        size: usize,
        sig_off: u64,
        sig_size: u64,
        cdhash: *mut u8,
        err: *mut c_char,
        errlen: usize,
    ) -> i32;
    fn a64x_cs_check(
        buf: *const u8,
        size: usize,
        sig_off: u64,
        sig_size: u64,
        cdhash: *mut u8,
        err: *mut c_char,
        errlen: usize,
    ) -> i32;
}

fn le32(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

fn be(b: &[u8], at: usize, n: usize) -> Option<u64> {
    Some(b.get(at..at + n)?.iter().fold(0, |v, &x| v << 8 | u64::from(x)))
}

/// The file's slices as (offset, size); a thin file is one.
fn slices(b: &[u8]) -> Vec<(usize, usize)> {
    let magic: [u8; 4] = match b.get(..4).and_then(|m| m.try_into().ok()) {
        Some(m) => m,
        None => return Vec::new(),
    };
    let (stride, wide) = match magic {
        THIN => return vec![(0, b.len())],
        FAT => (20, false),
        FAT64 => (32, true),
        _ => return Vec::new(),
    };
    let count = be(b, 4, 4).unwrap_or(0) as usize;
    (0..count)
        .filter_map(|i| {
            let at = 8 + stride * i;
            let (off, size) = if wide {
                (be(b, at + 8, 8)?, be(b, at + 16, 8)?)
            } else {
                (be(b, at + 8, 4)?, be(b, at + 12, 4)?)
            };
            Some((usize::try_from(off).ok()?, usize::try_from(size).ok()?))
        })
        .collect()
}

fn signature(slice: &[u8]) -> Option<(u64, u64)> {
    if slice.get(..4)? != THIN {
        return None;
    }
    let mut at = 32;
    for _ in 0..le32(slice, 16)? {
        if le32(slice, at)? == LC_CODE_SIGNATURE {
            return Some((u64::from(le32(slice, at + 8)?), u64::from(le32(slice, at + 12)?)));
        }
        at += le32(slice, at + 4)? as usize;
    }
    None
}

pub fn translated(mut t: Translated) -> Result<Translated, Error> {
    for (off, size) in slices(&t.bytes) {
        let Some(slice) = t.bytes.get_mut(off..off + size) else {
            continue;
        };
        let Some((sig_off, sig_size)) = signature(slice) else {
            continue;
        };
        let (mut old, mut new) = ([0u8; CDHASH], [0u8; CDHASH]);
        let mut err = [0 as c_char; ERR_LEN];
        // SAFETY: `slice` is `size` bytes, the cdhashes CDHASH and `err` ERR_LEN.
        let rc = unsafe {
            if a64x_cs_check(
                slice.as_ptr(),
                size,
                sig_off,
                sig_size,
                old.as_mut_ptr(),
                err.as_mut_ptr(),
                ERR_LEN,
            ) != 0
            {
                -1
            } else {
                slice[sig_off as usize - 1] ^= 0xff;
                a64x_cs_rehash(
                    slice.as_mut_ptr(),
                    size,
                    sig_off,
                    sig_size,
                    new.as_mut_ptr(),
                    err.as_mut_ptr(),
                    ERR_LEN,
                )
            }
        };
        if rc != 0 {
            return Err(Error::Refused(crate::ffi::message(&err)));
        }
        for s in t.slices.iter_mut().filter(|s| s.translated && s.cdhash_out == old) {
            s.cdhash_out = new;
        }
        break;
    }
    Ok(t)
}
