// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads an APFS container's newest checkpoint: the container layer (space manager, free queues, object map) and
//! the volumes it names. Sealed (headerless) trees are refused for now.

pub mod checkpoint;
pub mod node;
pub mod omap;
pub mod space;
pub mod walk;

use switch_apfs_format::io::BlockSource;

pub use checkpoint::Checkpoint;
pub use omap::{Omap, Volume};
pub use space::{QueuedFree, Space};

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
}
