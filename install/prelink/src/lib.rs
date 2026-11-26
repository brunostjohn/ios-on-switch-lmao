// SPDX-License-Identifier: GPL-3.0-or-later
//! Apple's kexts prelinked into this project's kernel, byte for byte as CI's prelinkers make it.

mod error;
mod fileset;
mod info;
mod macho;
mod pyint;
mod sites;
mod v17;

pub use error::Error;
pub use fileset::{FilesetInputs, fileset_pins, prelink_fileset};
pub use sites::{Site, load_sites};
pub use v17::{Inputs17, prelink_17};

#[cfg(test)]
mod tests;
