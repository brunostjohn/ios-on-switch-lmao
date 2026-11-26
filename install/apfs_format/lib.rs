// SPDX-License-Identifier: GPL-3.0-or-later
//! APFS on-disk layouts, from Apple's File System Reference (2020-06-22), and the block I/O the reader and writer
//! share.

pub mod btree;
pub mod container;
pub mod fs;
pub mod io;
pub mod object;
pub mod omap;
pub mod spaceman;
pub mod volume;

pub use object::{BLOCK_SIZE, Block, ObjPhys, checksum_ok, fletcher64, get, header, put, seal};

#[cfg(test)]
mod tests;
