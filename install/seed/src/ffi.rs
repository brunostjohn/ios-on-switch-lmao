// SPDX-License-Identifier: GPL-3.0-or-later
extern crate native_metal_translate;
extern crate switch_compile_desktop;

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::path::Path;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::error::{Result, SeedError};

const SWITCH_SEED_OK: c_int = 0;
const SWITCH_SEED_ERR_CANCELLED: c_int = 4;
const ERR_BUF_LEN: usize = 1024;

type CancelCheckFn = unsafe extern "C" fn(*mut c_void) -> c_int;

#[repr(C)]
struct DirectpackOpts {
    kind: *const c_char,
    qc_metallib: *const c_char,
    corpus_path: *const c_char,
    output_path: *const c_char,
    own_lib: *const c_char,
    lib_root: *const c_char,
    data_libs: *const c_char,
    label_prefix: *const c_char,
    scope: u64,
    cancel_check: Option<CancelCheckFn>,
    cancel_ctx: *mut c_void,
}

#[repr(C)]
struct NativeOpts {
    kind: *const c_char,
    libs_store: *const c_char,
    qc_metallib: *const c_char,
    stack_key: u64,
    output_dir: *const c_char,
    corpora: *const *const c_char,
    corpus_count: usize,
    derived: *const *const c_char,
    derived_count: usize,
    scope: u64,
    cancel_check: Option<CancelCheckFn>,
    cancel_ctx: *mut c_void,
}

unsafe extern "C" {
    fn switch_seed_directpack(opts: *const DirectpackOpts, err_buf: *mut c_char, err_cap: usize) -> c_int;
    fn switch_seed_native_bucket(opts: *const NativeOpts, err_buf: *mut c_char, err_cap: usize) -> c_int;
    fn nmb_keep_translations(bytes: u64);
    fn nmb_offline_scope_new() -> u64;
}

static CACHE_STATE: Mutex<usize> = Mutex::new(0);

/// Holds m2v's process-wide translation cache budget for the lifetime of a seed build, releasing it when the last
/// active build drops so APFS assembly and writes do not retain compiled shader caches.
pub struct TranslationCacheGuard;

impl TranslationCacheGuard {
    pub fn acquire(bytes: u64) -> Self {
        let _ = switch_compile_desktop::abi();
        let mut active = CACHE_STATE.lock().unwrap_or_else(|p| p.into_inner());
        *active += 1;
        // SAFETY: nmb_keep_translations updates an atomic byte budget.
        unsafe {
            nmb_keep_translations(bytes);
        }
        Self
    }
}

impl Drop for TranslationCacheGuard {
    fn drop(&mut self) {
        let mut active = CACHE_STATE.lock().unwrap_or_else(|p| p.into_inner());
        *active = active.saturating_sub(1);
        if *active == 0 {
            // SAFETY: clears retained m2v translation caches when no seed run is active.
            unsafe {
                nmb_keep_translations(0);
            }
        }
    }
}

pub fn init_runtime(translation_cache_bytes: u64) -> TranslationCacheGuard {
    TranslationCacheGuard::acquire(translation_cache_bytes)
}

pub fn desktop_compiler_abi() -> [u32; 3] {
    switch_compile_desktop::abi()
}

pub fn alloc_scope() -> u64 {
    // SAFETY: returns a fresh non-zero library scope identifier from an atomic counter.
    unsafe { nmb_offline_scope_new() }
}

unsafe extern "C" fn cancel_trampoline(ctx: *mut c_void) -> c_int {
    if ctx.is_null() {
        return 0;
    }
    // SAFETY: ctx points to the caller's AtomicBool for the synchronous duration of the FFI call.
    let flag = unsafe { &*(ctx as *const AtomicBool) };
    c_int::from(flag.load(Ordering::Relaxed))
}

fn to_cstring(s: &str, op: &'static str) -> Result<CString> {
    CString::new(s).map_err(|_| SeedError::Ffi {
        op,
        detail: format!("string contains interior NUL: {s:?}"),
    })
}

fn path_to_cstring(p: &Path, op: &'static str) -> Result<CString> {
    let s = p.to_str().ok_or_else(|| SeedError::Ffi {
        op,
        detail: format!("non-UTF-8 path: {}", p.display()),
    })?;
    to_cstring(s, op)
}

fn read_err_buf(buf: &[c_char]) -> String {
    // SAFETY: err_buf is zero-initialized and NUL-terminated by the C callee.
    unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned()
}

pub struct DirectpackArgs<'a> {
    pub kind: &'a str,
    pub qc_metallib: &'a Path,
    pub corpus_path: &'a Path,
    pub output_path: &'a Path,
    pub own_lib: Option<&'a Path>,
    pub lib_root: Option<&'a Path>,
    pub data_libs: Option<&'a Path>,
    pub label_prefix: Option<&'a str>,
    pub scope: u64,
    pub cancel: &'a AtomicBool,
}

pub fn directpack(args: &DirectpackArgs<'_>) -> Result<()> {
    const OP: &str = "directpack";
    let kind = to_cstring(args.kind, OP)?;
    let qc = path_to_cstring(args.qc_metallib, OP)?;
    let corpus = path_to_cstring(args.corpus_path, OP)?;
    let output = path_to_cstring(args.output_path, OP)?;
    let own = args.own_lib.map(|p| path_to_cstring(p, OP)).transpose()?;
    let lib_root = args.lib_root.map(|p| path_to_cstring(p, OP)).transpose()?;
    let data_libs = args.data_libs.map(|p| path_to_cstring(p, OP)).transpose()?;
    let label = args.label_prefix.map(|s| to_cstring(s, OP)).transpose()?;

    let opts = DirectpackOpts {
        kind: kind.as_ptr(),
        qc_metallib: qc.as_ptr(),
        corpus_path: corpus.as_ptr(),
        output_path: output.as_ptr(),
        own_lib: own.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        lib_root: lib_root.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        data_libs: data_libs.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        label_prefix: label.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        scope: args.scope,
        cancel_check: Some(cancel_trampoline),
        cancel_ctx: args.cancel as *const AtomicBool as *mut c_void,
    };

    let mut err_buf = [0 as c_char; ERR_BUF_LEN];
    // SAFETY: all CStrings and cancel pointer live across the synchronous FFI call.
    let rc = unsafe { switch_seed_directpack(&opts, err_buf.as_mut_ptr(), err_buf.len()) };
    if rc == SWITCH_SEED_OK {
        Ok(())
    } else if rc == SWITCH_SEED_ERR_CANCELLED {
        Err(SeedError::Cancelled)
    } else {
        let msg = read_err_buf(&err_buf);
        Err(SeedError::Ffi {
            op: OP,
            detail: if msg.is_empty() {
                format!("failed with status {rc}")
            } else {
                msg
            },
        })
    }
}

pub struct NativeBucketArgs<'a> {
    pub kind: &'a str,
    pub libs_store: &'a Path,
    pub qc_metallib: &'a Path,
    pub stack_key: u64,
    pub output_dir: &'a Path,
    pub corpora: &'a [&'a Path],
    pub derived: &'a [&'a Path],
    pub scope: u64,
    pub cancel: &'a AtomicBool,
}

pub fn native_bucket(args: &NativeBucketArgs<'_>) -> Result<()> {
    const OP: &str = "native_bucket";
    let kind = to_cstring(args.kind, OP)?;
    let store = path_to_cstring(args.libs_store, OP)?;
    let qc = path_to_cstring(args.qc_metallib, OP)?;
    let output = path_to_cstring(args.output_dir, OP)?;

    let corpus_c: Vec<CString> = args
        .corpora
        .iter()
        .map(|p| path_to_cstring(p, OP))
        .collect::<Result<_>>()?;
    let derived_c: Vec<CString> = args
        .derived
        .iter()
        .map(|p| path_to_cstring(p, OP))
        .collect::<Result<_>>()?;
    let corpus_ptrs: Vec<*const c_char> = corpus_c.iter().map(|c| c.as_ptr()).collect();
    let derived_ptrs: Vec<*const c_char> = derived_c.iter().map(|c| c.as_ptr()).collect();

    let opts = NativeOpts {
        kind: kind.as_ptr(),
        libs_store: store.as_ptr(),
        qc_metallib: qc.as_ptr(),
        stack_key: args.stack_key,
        output_dir: output.as_ptr(),
        corpora: if corpus_ptrs.is_empty() {
            std::ptr::null()
        } else {
            corpus_ptrs.as_ptr()
        },
        corpus_count: corpus_ptrs.len(),
        derived: if derived_ptrs.is_empty() {
            std::ptr::null()
        } else {
            derived_ptrs.as_ptr()
        },
        derived_count: derived_ptrs.len(),
        scope: args.scope,
        cancel_check: Some(cancel_trampoline),
        cancel_ctx: args.cancel as *const AtomicBool as *mut c_void,
    };

    let mut err_buf = [0 as c_char; ERR_BUF_LEN];
    // SAFETY: all CStrings, pointer arrays, and cancel pointer live across the synchronous FFI call.
    let rc = unsafe { switch_seed_native_bucket(&opts, err_buf.as_mut_ptr(), err_buf.len()) };
    if rc == SWITCH_SEED_OK {
        Ok(())
    } else if rc == SWITCH_SEED_ERR_CANCELLED {
        Err(SeedError::Cancelled)
    } else {
        let msg = read_err_buf(&err_buf);
        Err(SeedError::Ffi {
            op: OP,
            detail: if msg.is_empty() {
                format!("failed with status {rc}")
            } else {
                msg
            },
        })
    }
}
