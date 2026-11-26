// SPDX-License-Identifier: GPL-3.0-or-later
//! APFS test oracle harness combining the full 3-volume pipeline (`switch_apfs_write`, `switch_image`,
//! `switch_image_overlay`, `switch_image_bootstrap`, `switch_image_pretranslated`, `switch_apfs_seal::SealFill`)
//! with plan dumps, verification, negative-control tampering, and phase-boundary power-cut proofs.

pub mod compose;
pub mod inspect;
pub mod parse;
pub mod plan_dump;
pub mod power_cuts;
pub mod tamper;
pub mod verify;

use std::io;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Read(#[from] switch_apfs_read::ReadError),
    #[error(transparent)]
    Write(#[from] switch_apfs_write::WriteError),
    #[error(transparent)]
    Seal(#[from] switch_apfs_seal::SealError),
    #[error(transparent)]
    Image(#[from] switch_image::Error),
    #[error(transparent)]
    Overlay(#[from] switch_image_overlay::Error),
    #[error(transparent)]
    Bootstrap(#[from] switch_image_bootstrap::Error),
    #[error(transparent)]
    Pretranslated(#[from] switch_image_pretranslated::Error),
    #[error(transparent)]
    Index(#[from] switch_image_pretranslated::IndexError),
    #[error(transparent)]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error(transparent)]
    Geometry(#[from] switch_apfs_alloc::GeometryError),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    #[error("{0}")]
    Check(String),
}

pub type Result<T> = std::result::Result<T, Error>;

#[cfg(test)]
mod tests;
