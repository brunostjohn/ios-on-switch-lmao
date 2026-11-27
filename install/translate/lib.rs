// SPDX-License-Identifier: GPL-3.0-or-later
//! iOS 27's pretranslation, in process: a role's volume read straight from its decoded IPSW member, every arm64e
//! Mach-O translated to plain arm64 and re-signed, and for the SystemOS cryptex both dyld shared caches, byte for byte
//! as `translate/arm64e/pretranslate/translate_tree.py` makes them with the `arm64e` CLI. A role's delta is kept only
//! once the release's pins of the run accept it.

mod buffer;
mod cache;
pub mod edits;
mod ffi;
mod macho;
mod manifest;
mod mmap;
mod output;
#[cfg(switch_translate_perturb)]
mod perturb;
mod plan;
mod role;
mod trust;
pub mod volume;

use std::io;

pub use edits::Edit;
pub use ffi::CdhashPair;
pub use macho::{Slice as MachoSlice, translate_macho};
pub use role::{Delta, Inputs, Role, pair_lines, parse_pairs, translate, translate_file};
pub use trust::Pins;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Read(#[from] switch_apfs_read::ReadError),
    #[error(transparent)]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error(transparent)]
    TrustCache(#[from] switch_trustcache::Error),
    #[error("the edit table: {0}")]
    Edits(String),
    #[error("{path}: {message}")]
    Translate { path: String, message: String },
    #[error("the dyld cache plan: {0}")]
    Plan(String),
    #[error("refused: {0}")]
    Refused(String),
    #[error("{path}: {source}")]
    At { path: String, source: Box<Error> },
}

impl Error {
    pub(crate) fn at(self, path: &str) -> Self {
        match self {
            Error::Translate { .. } | Error::At { .. } => self,
            other => Error::At {
                path: path.to_owned(),
                source: Box::new(other),
            },
        }
    }
}

#[cfg(test)]
mod tests;
