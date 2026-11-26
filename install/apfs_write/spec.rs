// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_names::UnicodeVersion;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CheckpointAreas {
    /// The source's descriptor and data areas, where they are.
    Keep,
    /// New areas of these sizes in free space; the source's are freed.
    Sized { desc: u32, data: u32 },
}

impl CheckpointAreas {
    /// What `diskutil apfs resizeContainer` gave the Mac-built images, and newfs_apfs gives 1 TiB.
    pub const RESIZED: CheckpointAreas = CheckpointAreas::Sized {
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

#[derive(Clone, Debug)]
pub struct Owner {
    pub uid: u32,
    pub gid: u32,
    /// Permission bits only; the file type comes from the entry.
    pub mode: u16,
}

#[derive(Clone, Debug)]
pub enum Content {
    Dir(Vec<Entry>),
    File(Vec<u8>),
}

#[derive(Clone, Debug)]
pub struct Entry {
    pub name: String,
    pub owner: Owner,
    pub content: Content,
}

/// What a new volume holds. Files carry their bytes in memory for now.
#[derive(Clone, Debug)]
pub struct Tree {
    pub root: Owner,
    pub entries: Vec<Entry>,
}
