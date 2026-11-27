// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_names::UnicodeVersion;

use crate::tree::Tree;

/// What pins the source: the IPSW's System volume.
#[derive(Clone, Debug)]
pub struct SourcePins {
    pub system_uuid: [u8; 16],
}

/// A volume the writer adds. The pins (UUID, name, role, feature bits, Unicode version) come from the install spec
/// and the IPSW's pins; nothing here defaults them.
#[derive(Clone, Debug)]
pub struct VolumeSpec {
    pub uuid: [u8; 16],
    pub name: String,
    pub role: u16,
    pub features: u64,
    pub readonly_compatible_features: u64,
    /// ORed with NORMALIZATION_INSENSITIVE; the volume is case-sensitive.
    pub incompatible_features: u64,
    pub unicode: UnicodeVersion,
}

/// A volume and what it holds, in the order the container lists them after System.
pub struct NewVolume<'a> {
    pub spec: &'a VolumeSpec,
    pub tree: &'a Tree,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CheckpointAreas {
    /// What newfs_apfs and `resizeContainer` give the target's size (`Geometry::for_blocks(..).checkpoint`).
    #[default]
    Rule,
    /// The source's descriptor and data areas, where they are.
    Keep,
    /// New areas of these sizes in free space; the source's are freed.
    Sized { desc: u32, data: u32 },
}

impl CheckpointAreas {
    /// The areas every Mac-composed image carried: the rule at the 1 TiB it was composed at, which the fit kept.
    /// The only areas the device has booted from.
    pub const MAC_COMPOSED: CheckpointAreas = CheckpointAreas::Sized {
        desc: 280,
        data: 27_748,
    };
}

#[derive(Clone, Debug)]
pub struct WriteOptions {
    pub target_blocks: u64,
    pub areas: CheckpointAreas,
    /// Every new timestamp, in nanoseconds since 1970, so two builds are byte-identical.
    pub time_ns: u64,
    /// `apfs_formatted_by.id`, at most 31 bytes.
    pub formatted_by: String,
}
