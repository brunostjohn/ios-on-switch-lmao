// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::c_int;

unsafe extern "C" {
    fn switch_hello_cpp_check() -> c_int;
}

pub fn cpp_check() -> i32 {
    unsafe { switch_hello_cpp_check() }
}
