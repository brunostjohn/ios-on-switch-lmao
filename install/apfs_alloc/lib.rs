// SPDX-License-Identifier: GPL-3.0-or-later
//! Container geometry, the allocation map and the space manager built from it.

pub mod bitmap;
pub mod geometry;
pub mod space;

pub use bitmap::Bitmap;
pub use geometry::{Geometry, GeometryError};

#[cfg(test)]
mod tests;
