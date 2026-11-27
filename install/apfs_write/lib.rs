// SPDX-License-Identifier: GPL-3.0-or-later
//! Adds volumes to Apple's APFS container. Every block of the System volume stays at its own block number; the
//! container layer above it (space manager, object map, checkpoint) is rebuilt for the target's size, and the new
//! volumes are built from trees of inodes, names, xattrs and data streams.
//!
//! `plan` reads the source and decides every address and every metadata byte; `commit::write` then writes in an order
//! where nothing mounts until block 0, written last.

pub mod checkpoint;
pub mod commit;
pub mod content;
pub mod copy;
pub mod extents;
mod ids;
pub mod inode;
pub mod keys;
pub mod place;
pub mod plan;
pub mod records;
pub mod spec;
pub mod target;
pub mod tree;
pub mod volume;

pub use commit::{Phase, PhaseStats, write, write_until};
pub use content::{Content, Fill, Sequential, SourceExtent};
pub use copy::{Copied, copy_subtree};
pub use plan::{Plan, Summary, plan};
pub use spec::{CheckpointAreas, NewVolume, SourcePins, VolumeSpec, WriteOptions};
pub use tree::{Attrs, InodeRef, Kind, StreamRef, Times, Tree, Xattr, XattrValue};

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
    #[error("no room for {0} more blocks")]
    Full(u64),
}

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_commit;
#[cfg(test)]
mod tests_records;
#[cfg(test)]
mod tests_refusals;
