// SPDX-License-Identifier: GPL-3.0-or-later
//! Card images for the installer's tests: sparse images of any size, a write recorder for power cuts, a GPT writer
//! for synthetic cards, and the owner's 1 TB card.

pub mod image;
pub mod owner;
pub mod tables;

pub use image::{Event, Recorder, Sector, SparseImage};
