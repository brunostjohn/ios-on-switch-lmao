// SPDX-License-Identifier: GPL-3.0-or-later

pub mod archives;
pub mod directlink;
pub mod fetchfree;
pub mod headers;
pub mod iterated_long;
pub mod iterated_short;
pub mod iterated_ucode;
pub mod kernels;
pub mod packfile;
pub mod packfile_encode;
pub mod packfile_records;
pub mod packfile_sections;
pub mod pipeline_build;
pub mod probelog;
pub mod records;
pub mod segment_methods;
pub mod segments;
pub mod splits;

pub use directlink::{LinkConfig, LinkOutput, link_buckets};
pub use packfile_encode::{pack_bytes, rules_identity, sections_line, stamp_pack_bytes, stamp_pack_file};

#[derive(Debug, thiserror::Error)]
pub enum LinkError {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("{0}")]
    Invalid(String),
}

pub type Result<T> = std::result::Result<T, LinkError>;

#[cfg(test)]
mod tests;
