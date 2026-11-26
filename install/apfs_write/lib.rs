// SPDX-License-Identifier: GPL-3.0-or-later
//! Adds volumes to Apple's APFS container. Every block of the System volume stays at its own block number; the
//! container layer above it (space manager, object map, checkpoint) is rebuilt for the target's size, and the new
//! volumes are built from a planned model.
//!
//! `plan` reads the source and decides every address; `commit::write` then writes in an order where nothing mounts
//! until block 0, written last.

pub mod checkpoint;
pub mod commit;
pub mod plan;
pub mod records;
pub mod spec;
pub mod volume;

pub use commit::{Phase, write, write_until};
pub use plan::{Plan, Summary, plan};
pub use spec::{CheckpointAreas, Content, Entry, Owner, SourcePins, Tree, VolumeSpec, WriteOptions};

#[derive(Debug, thiserror::Error)]
pub enum WriteError {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Read(#[from] switch_apfs_read::ReadError),
    #[error(transparent)]
    Geometry(#[from] switch_apfs_alloc::GeometryError),
    #[error(transparent)]
    Space(#[from] switch_apfs_alloc::space::SpaceError),
    #[error(transparent)]
    Build(#[from] switch_apfs_btree::BuildError),
    #[error(transparent)]
    Name(#[from] switch_apfs_names::NameError),
    #[error("source: {0}")]
    Source(&'static str),
    #[error("refused: {0}")]
    Refused(String),
    #[error("{0}")]
    Spec(String),
    #[error("no run of {0} free blocks")]
    Full(u64),
}

#[cfg(test)]
mod tests;
