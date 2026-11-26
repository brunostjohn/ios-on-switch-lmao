// SPDX-License-Identifier: GPL-3.0-or-later
//! c/translate.h and c/plan.h.

use std::ffi::{c_char, c_int, c_void};

pub const CDHASH: usize = 20;
pub const SLICES: usize = 16;
pub const ERR_LEN: usize = 256;

pub const SLICE_TRANSLATED: u32 = 1;
pub const SLICE_FALLBACK: u32 = 2;

#[repr(C)]
pub struct Edit {
    pub fileoff: u64,
    pub stock: *const u8,
    pub stock_len: usize,
    pub at: usize,
    pub patched: *const u8,
    pub patched_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Slice {
    pub status: u32,
    pub cdhash_in: [u8; CDHASH],
    pub cdhash_out: [u8; CDHASH],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct CdhashPair {
    pub old: [u8; CDHASH],
    pub now: [u8; CDHASH],
}

#[repr(C)]
pub struct PlanFile {
    pub suffix: *const c_char,
    pub data: *const u8,
    pub size: usize,
}

unsafe extern "C" {
    pub fn switch_translate_macho(
        input: *const u8,
        size: usize,
        out: *mut u8,
        edits: *const Edit,
        nedits: usize,
        slices: *mut Slice,
        err: *mut c_char,
    ) -> c_int;

    pub fn switch_translate_dsc(
        plan: *const u8,
        plan_size: usize,
        input: *const u8,
        size: usize,
        pairs: *const CdhashPair,
        npairs: usize,
        out: *mut u8,
        cdhash: *mut u8,
        err: *mut c_char,
    ) -> c_int;

    pub fn switch_plan_make(
        files: *const PlanFile,
        nfiles: usize,
        plan: *mut *mut u8,
        plan_size: *mut usize,
        log: *mut *mut c_char,
    ) -> c_int;

    pub fn switch_plan_release(buffer: *mut c_void);
}

pub fn message(err: &[c_char; ERR_LEN]) -> String {
    let bytes: Vec<u8> = err.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}
