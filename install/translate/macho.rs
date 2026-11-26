// SPDX-License-Identifier: GPL-3.0-or-later
//! One Mach-O file translated as `arm64e pretranslate` translates it.

use std::ffi::c_char;

use crate::Error;
use crate::buffer::Buffer;
use crate::edits::Edit;
use crate::ffi;

pub const MAGICS: [[u8; 4]; 3] = [*b"\xcf\xfa\xed\xfe", *b"\xca\xfe\xba\xbe", *b"\xca\xfe\xba\xbf"];

pub fn is_macho(head: &[u8]) -> bool {
    head.len() >= 4 && MAGICS.iter().any(|m| head[..4] == m[..])
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slice {
    pub translated: bool,
    pub cdhash_in: [u8; ffi::CDHASH],
    pub cdhash_out: [u8; ffi::CDHASH],
}

pub struct Translated {
    pub bytes: Buffer,
    pub slices: Vec<Slice>,
}

impl Translated {
    pub fn translated(&self) -> impl Iterator<Item = &Slice> {
        self.slices.iter().filter(|s| s.translated)
    }
}

/// `input` (16-byte aligned) with its arm64 slices translated, checked and re-signed, and what became of each slice.
/// A file that isn't a Mach-O has no slices.
pub fn translate(path: &str, input: &[u8], edits: &[Edit]) -> Result<Translated, Error> {
    let raw: Vec<ffi::Edit> = edits.iter().map(Edit::raw).collect();
    let mut out = Buffer::zeroed(input.len());
    let mut slices = [ffi::Slice::default(); ffi::SLICES];
    let mut err = [0 as c_char; ffi::ERR_LEN];
    // SAFETY: the buffers are as long as the lengths given, `slices` holds SLICES entries, `err` ERR_LEN bytes, and
    // the edits' pointers borrow `edits` for the call.
    let n = unsafe {
        ffi::switch_translate_macho(
            input.as_ptr(),
            input.len(),
            out.as_mut_ptr(),
            raw.as_ptr(),
            raw.len(),
            slices.as_mut_ptr(),
            err.as_mut_ptr(),
        )
    };
    let Ok(n) = usize::try_from(n) else {
        return Err(Error::Translate {
            path: path.to_owned(),
            message: ffi::message(&err),
        });
    };
    let slices = slices[..n]
        .iter()
        .map(|s| Slice {
            translated: s.status == ffi::SLICE_TRANSLATED,
            cdhash_in: s.cdhash_in,
            cdhash_out: s.cdhash_out,
        })
        .collect();
    Ok(Translated { bytes: out, slices })
}
