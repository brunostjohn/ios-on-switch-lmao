// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{c_char, c_int, c_void};

/// `A64X_NCLASSES` (translate/arm64e/a64x/a64x.h). `switch_a64x_kext_result_size` catches a change.
pub const CLASSES: usize = 38;

#[repr(C)]
pub struct Kext {
    pub sections: *const [u64; 2],
    pub nsections: usize,
    pub starts: *const u64,
    pub nstarts: usize,
}

#[repr(C)]
pub struct Kexts {
    pub original: *const u8,
    pub text: *mut u8,
    pub size: usize,
    pub va: u64,
    pub stubs_va: u64,
    pub stubs: *mut u32,
    pub stub_cap: u32,
    pub stub_words: u32,
}

#[repr(C)]
#[derive(Clone)]
pub struct Stats {
    pub rewrite1: [u64; CLASSES],
    pub stubs: [u64; CLASSES],
    pub left: [u64; CLASSES],
    pub stub_words: u64,
    pub stubs_spill: u64,
}

#[repr(C)]
pub struct KextResult {
    pub words: u64,
    pub changed: u64,
    pub island_words: u64,
    pub stub_first: u32,
    pub stub_end: u32,
    pub stats: Stats,
}

#[repr(C)]
#[derive(Default)]
pub struct Fips {
    pub text_vmaddr: u64,
    pub text_fileoff: u64,
    pub text_size: u64,
    pub mac_vmaddr: u64,
    pub mac_fileoff: u64,
}

pub type Island = unsafe extern "C" fn(ctx: *mut c_void, va: u64, words: u64);

unsafe extern "C" {
    pub fn switch_a64x_kext_translate(
        kexts: *mut Kexts,
        kext: *const Kext,
        island: Option<Island>,
        ctx: *mut c_void,
        result: *mut KextResult,
    ) -> c_int;
    pub fn switch_a64x_kext_result_size() -> usize;
    pub fn a64x_class_name(class: c_int) -> *const c_char;
    pub fn a64x_fips_find(header: *const u8, len: usize, fips: *mut Fips) -> c_int;
    pub fn a64x_fips_rewrite(original: *const u8, translated: *const u8, size: usize, mac: *mut u8) -> c_int;
}
