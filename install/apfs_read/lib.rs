// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads an APFS container: its newest checkpoint and container layer (space manager, free queues, object map), the
//! volumes it names, and each volume's file system, sealed or not. Records come raw or decoded, and as a path-level
//! walk that carries what a verbatim copy needs. `audit` checks that every used block has exactly one owner.

pub mod audit;
pub mod catalog;
pub mod checkpoint;
pub mod fs;
pub mod node;
pub mod omap;
pub mod space;
pub mod stream;
pub mod subtree;
pub mod walk;

use switch_apfs_format::io::BlockSource;

pub use audit::{Audit, audit};
pub use catalog::{DirRec, FileExtent, FileInfo, Inode, PhysExtent, Record, SiblingLink, XField, Xattr, XattrData};
pub use checkpoint::Checkpoint;
pub use fs::{Fs, RawRecord, Seal};
pub use omap::{Omap, Volume};
pub use space::{QueuedFree, Space};
pub use stream::Compression;
pub use subtree::{Entry, Step, Stream};

#[derive(Debug, thiserror::Error)]
pub enum ReadError {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("{0}")]
    Invalid(String),
    #[error("not supported: {0}")]
    Unsupported(String),
}

impl ReadError {
    pub fn invalid(what: impl Into<String>) -> Self {
        ReadError::Invalid(what.into())
    }
}

macro_rules! invalid {
    ($($arg:tt)*) => {
        $crate::ReadError::Invalid(format!($($arg)*))
    };
}
pub(crate) use invalid;

pub struct Container {
    pub checkpoint: Checkpoint,
    pub space: Space,
    pub omap: Omap,
    pub volumes: Vec<Volume>,
}

impl Container {
    pub fn open(src: &dyn BlockSource) -> Result<Self, ReadError> {
        let checkpoint = checkpoint::read(src)?;
        let space = space::read(src, &checkpoint)?;
        let omap = Omap::read(src, checkpoint.nx.omap_oid.get())?;
        let volumes = omap::volumes(src, &checkpoint.nx, &omap)?;
        Ok(Self {
            checkpoint,
            space,
            omap,
            volumes,
        })
    }

    pub fn blocks(&self) -> u64 {
        self.checkpoint.nx.block_count.get()
    }

    /// A volume by its UUID.
    pub fn volume(&self, uuid: [u8; 16]) -> Option<&Volume> {
        self.volumes.iter().find(|v| v.sb.vol_uuid == uuid)
    }
}

#[cfg(test)]
mod tests_records;
#[cfg(test)]
mod tests_walk;
