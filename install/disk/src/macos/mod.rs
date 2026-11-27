// SPDX-License-Identifier: GPL-3.0-or-later
//! The hold on macOS rests on three things macOS does to a card:
//! - DiskArbitration mounts APFS volumes again whenever their store's writer closes, so a mount-approval callback
//!   refuses every mount on the disk for as long as it is held, and through the re-probe after the descriptor closes.
//! - Spotlight indexes whatever is mounted, so nothing on the disk stays mounted while it is held.
//! - DiskArbitration describes a disk only after it has probed it, so `diskutil` asked straight after a write races
//!   it; release and eject wait for IOKit to go quiet and for DiskArbitration to announce each new partition.
mod arbitration;
mod device;
mod iokit;
mod list;
mod mounts;

#[cfg(all(test, feature = "device-tests"))]
mod device_tests;

use std::collections::HashSet;
use std::fs::File;
use std::path::PathBuf;
use std::time::Duration;

pub(crate) use arbitration::Arbiter;
pub(crate) use device::flush;
pub(crate) use list::list;

use crate::OpenOptions;
use crate::error::{Error, Result};
use crate::model::Disk;
use crate::raw::Geometry;

const REPROBE_TIMEOUT: Duration = Duration::from_secs(30);

pub(crate) struct Hold {
    arbiter: Arbiter,
    root: u64,
    bsd: String,
    remount: Vec<String>,
}

pub(crate) fn open(disk: &Disk, options: &OpenOptions) -> Result<(File, Geometry, Hold)> {
    let changed = || Error::Changed(disk.path.display().to_string());
    let root = list::parse_id(&disk.id).ok_or_else(changed)?;
    let bsd = disk
        .path
        .file_name()
        .ok_or_else(changed)?
        .to_string_lossy()
        .into_owned();
    let arbiter = Arbiter::hold(root)?;
    let mut hold = Hold {
        arbiter,
        root,
        bsd,
        remount: Vec::new(),
    };
    if let Err(e) = hold.unmount_all(options.force_unmount) {
        let _ = hold.restore();
        return Err(e);
    }
    let raw = PathBuf::from(format!("/dev/r{}", hold.bsd));
    let opened = device::open_raw(&raw, options.privilege).and_then(|f| Ok((device::geometry(&f, &raw)?, f)));
    match opened {
        Ok((geometry, file)) => Ok((file, geometry, hold)),
        Err(e) => {
            let _ = hold.restore();
            Err(e)
        }
    }
}

impl Hold {
    pub(crate) fn release(self, file: File) -> Result<()> {
        let flushed = device::flush(&file);
        self.close_and_settle(file)?;
        self.restore()?;
        flushed.map_err(|e| Error::io("flush before release", e))
    }

    pub(crate) fn eject(self, file: File) -> Result<()> {
        let flushed = device::flush(&file);
        self.close_and_settle(file)?;
        flushed.map_err(|e| Error::io("flush before eject", e))?;
        self.arbiter.eject(&self.bsd)
    }

    fn unmount_all(&mut self, force: bool) -> Result<()> {
        let media = iokit::all_media()?;
        let mounts = mounts::mounts().map_err(|e| Error::io("list mounted volumes", e))?;
        for mount in mounts {
            let Some(m) = media.iter().find(|m| m.bsd == mount.bsd && m.is_on(self.root)) else {
                continue;
            };
            self.arbiter.unmount(&m.bsd, force)?;
            self.remount.push(m.identity());
        }
        Ok(())
    }

    /// Closes the descriptor and waits for macOS to rebuild what the exclusive open tore down, with mounts still
    /// refused.
    fn close_and_settle(&self, file: File) -> Result<()> {
        let held: HashSet<u64> = self.on_disk()?.iter().map(|m| m.id).collect();
        let mark = self.arbiter.mark();
        drop(file);
        iokit::wait_quiet(&self.bsd, REPROBE_TIMEOUT)?;
        let fresh: Vec<String> = self
            .on_disk()?
            .into_iter()
            .filter(|m| !held.contains(&m.id))
            .map(|m| m.bsd)
            .collect();
        self.arbiter.wait_appeared(&fresh, mark, REPROBE_TIMEOUT)
    }

    /// Ends the refusal and mounts again the volumes that were mounted at open and still exist. New volumes stay as
    /// DiskArbitration leaves them.
    fn restore(&self) -> Result<()> {
        self.arbiter.stop_refusing();
        let mounted: HashSet<String> = mounts::mounts()
            .map_err(|e| Error::io("list mounted volumes", e))?
            .into_iter()
            .map(|m| m.bsd)
            .collect();
        for media in self.on_disk()? {
            if !self.remount.contains(&media.identity()) || mounted.contains(&media.bsd) {
                continue;
            }
            if let Err(e) = self.arbiter.mount(&media.bsd) {
                let now = mounts::mounts().map_err(|e| Error::io("list mounted volumes", e))?;
                if !now.iter().any(|m| m.bsd == media.bsd) {
                    return Err(e);
                }
            }
        }
        Ok(())
    }

    fn on_disk(&self) -> Result<Vec<iokit::Media>> {
        Ok(iokit::all_media()?.into_iter().filter(|m| m.is_on(self.root)).collect())
    }
}
