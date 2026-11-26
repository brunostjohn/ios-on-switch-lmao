// SPDX-License-Identifier: GPL-3.0-or-later
//! The dyld shared cache's translation plan, as `arm64e plan` makes it (c/plan.c).

use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::Mutex;

use crate::{Error, ffi};

/// The planner keeps its state in statics.
static PLANNER: Mutex<()> = Mutex::new(());

pub struct Plan {
    pub bytes: Vec<u8>,
    /// What the planner reports: sites, holes, placements and its own check.
    pub log: String,
}

/// `files`: the main cache file (suffix "") first, then each subcache by its suffix, each 16-byte aligned.
pub fn make(cache: &str, files: &[(&str, &[u8])]) -> Result<Plan, Error> {
    let suffixes = files
        .iter()
        .map(|(s, _)| CString::new(*s).map_err(|_| Error::Plan(format!("{cache}: a suffix holds a NUL"))))
        .collect::<Result<Vec<_>, _>>()?;
    let raw: Vec<ffi::PlanFile> = files
        .iter()
        .zip(&suffixes)
        .map(|((_, data), suffix)| ffi::PlanFile {
            suffix: suffix.as_ptr(),
            data: data.as_ptr(),
            size: data.len(),
        })
        .collect();
    let mut plan: *mut u8 = std::ptr::null_mut();
    let mut size = 0usize;
    let mut log: *mut c_char = std::ptr::null_mut();
    let rc = {
        let _only = PLANNER.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        // SAFETY: `raw` and the buffers and suffixes it points at outlive the call; the out-pointers are valid.
        unsafe { ffi::switch_plan_make(raw.as_ptr(), raw.len(), &mut plan, &mut size, &mut log) }
    };
    // SAFETY: the planner returns a malloc'd buffer of `size` bytes (or null) and a NUL-terminated log, each released
    // here exactly once.
    let (bytes, log) = unsafe {
        let bytes = if plan.is_null() {
            Vec::new()
        } else {
            std::slice::from_raw_parts(plan, size).to_vec()
        };
        let text = if log.is_null() {
            String::new()
        } else {
            CStr::from_ptr(log).to_string_lossy().into_owned()
        };
        ffi::switch_plan_release(plan.cast::<c_void>());
        ffi::switch_plan_release(log.cast::<c_void>());
        (bytes, text)
    };
    if rc != 0 {
        return Err(Error::Plan(format!("{cache}: the planner refused:\n{log}")));
    }
    Ok(Plan { bytes, log })
}
