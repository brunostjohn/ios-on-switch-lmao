// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{CStr, c_char, c_int};

unsafe extern "C" {
    fn switch_hello_greeting(buffer: *mut c_char, size: usize) -> c_int;
}

pub fn greeting() -> String {
    let mut buffer = [0 as c_char; 64];
    let written = unsafe { switch_hello_greeting(buffer.as_mut_ptr(), buffer.len()) };
    assert!(written >= 0, "snprintf failed");
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}
