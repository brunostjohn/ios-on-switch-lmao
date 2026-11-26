// SPDX-License-Identifier: GPL-3.0-or-later
//! Readers of what Linux and Windows report, built on every OS so their tests run on any machine.

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub mod mounts;
#[cfg_attr(not(windows), allow(dead_code))]
pub mod storage;
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub mod sysfs;

#[cfg(test)]
mod storage_tests;
#[cfg(all(test, unix))]
mod sysfs_tests;
