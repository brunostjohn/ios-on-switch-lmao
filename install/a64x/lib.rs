// SPDX-License-Identifier: GPL-3.0-or-later
//! The iOS 27 kext prelink's native steps, `arm64e kext`, `fips` and `fips-mac`, on buffers: our C translator
//! (translate/arm64e) compiled into the installer.

mod ffi;
pub mod fips;
mod kext;

pub use kext::{CLASSES, Error, Island, Kext, KextResult, Stats, Translated, class_name, stub_cap, translate_kexts};

#[cfg(test)]
mod tests;
