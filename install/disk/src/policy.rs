// SPDX-License-Identifier: GPL-3.0-or-later
use std::fmt;
use std::path::Path;

use crate::model::Disk;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    System,
    Internal,
    ReadOnly,
    NoMedia,
}

impl fmt::Display for Refusal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Refusal::System => "it holds a volume this computer is running from",
            Refusal::Internal => "it is a fixed internal disk",
            Refusal::ReadOnly => "it is write-protected",
            Refusal::NoMedia => "there is no card in it",
        })
    }
}

impl Disk {
    /// Why this disk can't be picked, or `None` when it can. A card in a built-in reader is internal but removable,
    /// so it stays pickable.
    pub fn refusal(&self) -> Option<Refusal> {
        if self.system {
            Some(Refusal::System)
        } else if self.internal && !self.removable {
            Some(Refusal::Internal)
        } else if self.read_only {
            Some(Refusal::ReadOnly)
        } else if self.size == 0 {
            Some(Refusal::NoMedia)
        } else {
            None
        }
    }

    pub fn pickable(&self) -> bool {
        self.refusal().is_none()
    }
}

#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
/// A mount point that only a disk the running macOS needs can hold: its root and the boot volume group's volumes.
pub fn is_macos_system_mount(mount_point: &Path) -> bool {
    mount_point == Path::new("/")
        || mount_point.starts_with("/System/Volumes")
        || mount_point.starts_with("/private/var/vm")
}

#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
/// A mount point a running Linux needs, or the `[SWAP]` marker for an active swap area.
pub fn is_linux_system_mount(mount_point: &Path) -> bool {
    const SYSTEM: [&str; 9] = [
        "/",
        "/boot",
        "/boot/efi",
        "/efi",
        "/usr",
        "/var",
        "/home",
        "/nix",
        "[SWAP]",
    ];
    SYSTEM.iter().any(|m| mount_point == Path::new(m))
}
