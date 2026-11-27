// SPDX-License-Identifier: GPL-3.0-or-later
//! The size picker's rules: what it offers by default and the least it accepts for an image.

/// New entries start and end on these boundaries (hekate's AU_ALIGN_SECTORS).
pub const ALIGN_SECTORS: u64 = 0x8000;
pub const ALIGN_BYTES: u64 = ALIGN_SECTORS * 512;
pub const DEFAULT_IOS_BYTES: u64 = 64 << 30;
pub const FLOOR_IOS_BYTES: u64 = 16 << 30;
/// Room left above what the image's container uses.
pub const HEADROOM_BYTES: u64 = 4 << 30;

/// The least the picker accepts for an image whose container uses `image_used` bytes.
pub fn minimum_ios_bytes(image_used: u64) -> u64 {
    align_up(
        FLOOR_IOS_BYTES.max(image_used.saturating_add(HEADROOM_BYTES)),
        ALIGN_BYTES,
    )
}

/// What the picker offers on a card: `max` is the most the boot volume can give while keeping its files.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    pub min: u64,
    pub default: u64,
    pub max: u64,
}

impl Limits {
    pub fn new(image_used: u64, max: u64) -> Limits {
        let min = minimum_ios_bytes(image_used);
        let max = align_down(max, ALIGN_BYTES);
        Limits {
            min,
            default: DEFAULT_IOS_BYTES.max(min).min(max),
            max,
        }
    }

    /// False when the card can't hold even the minimum.
    pub fn possible(&self) -> bool {
        self.min <= self.max
    }
}

pub fn align_down(value: u64, to: u64) -> u64 {
    value - value % to
}

pub fn align_up(value: u64, to: u64) -> u64 {
    value.div_ceil(to) * to
}
