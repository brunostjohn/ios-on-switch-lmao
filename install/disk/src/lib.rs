// SPDX-License-Identifier: GPL-3.0-or-later
//! The raw device layer the installer writes a card through: list the disks, then open one for exclusive,
//! uncached access with every volume on it unmounted and the OS kept from mounting it again until release.
mod align;
mod error;
mod model;
mod parse;
mod policy;
mod raw;
mod writer;

#[cfg(target_os = "linux")]
mod linux;
#[cfg(target_os = "macos")]
mod macos;
#[cfg(windows)]
mod windows;

#[cfg(target_os = "linux")]
use linux as platform;
#[cfg(target_os = "macos")]
use macos as platform;
#[cfg(windows)]
use windows as platform;

#[cfg(test)]
mod tests;

pub use align::AlignedBuf;
pub use error::{Error, Result};
pub use model::{Bus, Disk, Partition, Volume};
pub use policy::Refusal;
pub use raw::RawDisk;
pub use writer::Writer;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Privilege {
    /// Open the disk directly when this process may (root, administrator, or a disk image the user attached), and
    /// ask the OS's helper otherwise: authopen on macOS, udisks on Linux.
    #[default]
    Least,
    /// Always go through the helper.
    Helper,
}

#[derive(Clone, Debug, Default)]
pub struct OpenOptions {
    /// Unmount volumes even when a program has files open on them.
    pub force_unmount: bool,
    pub privilege: Privilege,
}

/// Every whole disk, internal ones included; [`Disk::refusal`] says which can't be picked.
pub fn list() -> Result<Vec<Disk>> {
    platform::list()
}

/// Opens `disk` for writing. The disk is looked up again first: one removed or replaced since it was listed, or one
/// that can't be picked, is refused before anything on it is unmounted.
pub fn open(disk: &Disk, options: &OpenOptions) -> Result<RawDisk> {
    let current = platform::list()?
        .into_iter()
        .find(|d| d.id == disk.id && d.path == disk.path)
        .ok_or_else(|| Error::Changed(disk.path.display().to_string()))?;
    if let Some(reason) = current.refusal() {
        return Err(Error::Refused {
            path: current.path.display().to_string(),
            reason,
        });
    }
    let (file, geometry, hold) = platform::open(&current, options)?;
    Ok(RawDisk::new(current, file, geometry, hold))
}
