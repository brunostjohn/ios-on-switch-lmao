// SPDX-License-Identifier: GPL-3.0-or-later
//! Card images for switch_partition's tests: FAT32 volumes macOS formats with files placed cluster by cluster, a
//! reader of their own, the kernel's rules, and power cuts over a recorded run.

pub mod cards;
pub mod cuts;
pub mod fat32;
pub mod fixtures;
pub mod macos;
pub mod oracle;
pub mod reader;
pub mod scenario;
