// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{CStr, c_int, c_void};

use crate::ffi;

pub use ffi::{CLASSES, Stats};

/// `SWITCH_A64X_KEXT_STUB_CAP`, the one the CLI that made CI's kernel used: the stub region `arm64e kext` gives the
/// kexts, in words. A stub past it leaves its site untranslated, so the cap is part of the output.
pub fn stub_cap() -> u32 {
    ffi::switch_a64x_kext_stub_cap
}

/// One kext's code, as `emit26.py` lists it for `arm64e kext`.
pub struct Kext<'a> {
    /// `[va, size]` of each instruction section.
    pub sections: &'a [[u64; 2]],
    /// `LC_FUNCTION_STARTS`, as addresses.
    pub starts: &'a [u64],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Island {
    pub va: u64,
    pub words: u64,
}

pub struct KextResult {
    pub words: u64,
    pub changed: u64,
    pub island_words: u64,
    pub stub_first: u32,
    pub stub_end: u32,
    pub stats: Stats,
    /// Data runs inside the kext's instruction sections, in address order.
    pub islands: Vec<Island>,
}

pub struct Translated {
    pub text: Vec<u8>,
    /// The stub region's words, little-endian.
    pub stubs: Vec<u8>,
    pub kexts: Vec<KextResult>,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("the translator ran out of memory")]
    OutOfMemory,
}

unsafe extern "C" fn push_island(ctx: *mut c_void, va: u64, words: u64) {
    let islands = unsafe { &mut *ctx.cast::<Vec<Island>>() };
    islands.push(Island { va, words });
}

/// `arm64e kext`: the kexts' coalesced text, `original` laid out at `va`, translated kext by kext into one stub region
/// at `stubs_va`.
pub fn translate_kexts(original: &[u8], va: u64, stubs_va: u64, kexts: &[Kext]) -> Result<Translated, Error> {
    assert_eq!(
        unsafe { ffi::switch_a64x_kext_result_size() },
        size_of::<ffi::KextResult>(),
        "switch_a64x_kext_result's layout differs from ffi::KextResult"
    );
    let mut text = original.to_vec();
    let stub_cap = stub_cap();
    let mut stubs = vec![0u32; stub_cap as usize];
    let mut region = ffi::Kexts {
        original: original.as_ptr(),
        text: text.as_mut_ptr(),
        size: original.len(),
        va,
        stubs_va,
        stubs: stubs.as_mut_ptr(),
        stub_cap,
        stub_words: 0,
    };
    let mut results = Vec::with_capacity(kexts.len());
    for kext in kexts {
        let raw = ffi::Kext {
            sections: kext.sections.as_ptr(),
            nsections: kext.sections.len(),
            starts: kext.starts.as_ptr(),
            nstarts: kext.starts.len(),
        };
        let mut islands = Vec::<Island>::new();
        let mut result = std::mem::MaybeUninit::<ffi::KextResult>::uninit();
        let status = unsafe {
            ffi::switch_a64x_kext_translate(
                &mut region,
                &raw,
                Some(push_island),
                (&mut islands as *mut Vec<Island>).cast(),
                result.as_mut_ptr(),
            )
        };
        if status != 0 {
            return Err(Error::OutOfMemory);
        }
        let result = unsafe { result.assume_init() };
        results.push(KextResult {
            words: result.words,
            changed: result.changed,
            island_words: result.island_words,
            stub_first: result.stub_first,
            stub_end: result.stub_end,
            stats: result.stats,
            islands,
        });
    }
    let stubs = stubs[..region.stub_words as usize]
        .iter()
        .flat_map(|w| w.to_le_bytes())
        .collect();
    Ok(Translated {
        text,
        stubs,
        kexts: results,
    })
}

/// `a64x_class_name`: the name `arm64e kext` prints for an instruction class, `?` past the last.
pub fn class_name(class: usize) -> &'static str {
    let Ok(class) = c_int::try_from(class) else {
        return "?";
    };
    unsafe { CStr::from_ptr(ffi::a64x_class_name(class)) }
        .to_str()
        .unwrap_or("?")
}
