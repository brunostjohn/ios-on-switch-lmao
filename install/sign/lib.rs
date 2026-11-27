// SPDX-License-Identifier: GPL-3.0-or-later
//! Ad hoc code signatures byte for byte as `/usr/bin/codesign -s -` makes them with `--timestamp=none` and
//! SHA-256 only: an embedded signature for a thin 64-bit executable, and a detached one for any other file.
//! Anything `codesign` would sign differently from what is measured here is refused.

mod blob;
mod code_directory;
mod detached;
mod embedded;
mod entitlements;
mod macho;

pub use code_directory::CDHASH_LEN;
pub use detached::{Detached, sign_detached};
pub use embedded::{Embedded, Metadata, embedded_entitlements, sign_macho};

/// `-o runtime`.
pub const RUNTIME: u32 = 0x1_0000;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("malformed Mach-O: {0}")]
    MachO(&'static str),
    #[error("malformed code signature: {0}")]
    Signature(&'static str),
    #[error("unsupported: {0}")]
    Unsupported(&'static str),
    #[error("entitlements: {0}")]
    Entitlements(String),
}

pub struct Signed {
    /// The signed file, or for a detached signature, the signature.
    pub bytes: Vec<u8>,
    pub cdhash: [u8; CDHASH_LEN],
}
