// SPDX-License-Identifier: MIT
extern crate switch_compile_desktop;

use std::ffi::{CString, c_char, c_int};
use std::process::ExitCode;

unsafe extern "C" {
    fn switch_parity_run(argc: c_int, argv: *mut *mut c_char) -> c_int;
}

fn main() -> ExitCode {
    let args: Vec<CString> = std::env::args()
        .map(|a| CString::new(a).expect("NUL in argv"))
        .collect();
    let mut ptrs: Vec<*mut c_char> = args.iter().map(|s| s.as_ptr() as *mut c_char).collect();
    let rc = unsafe { switch_parity_run(ptrs.len() as c_int, ptrs.as_mut_ptr()) };
    if rc == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::from(rc as u8)
    }
}
