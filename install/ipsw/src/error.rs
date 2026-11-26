// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;

use switch_catalog::Container;

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0} is an Apple Encrypted Archive, which switch_aea decrypts")]
    Encrypted(String),
    #[error("{member}: {got} stored bytes, pinned {expected}")]
    Stored { member: String, got: u64, expected: u64 },
    #[error("{member}: the deflate stream {problem}")]
    Deflate { member: String, problem: String },
    #[error("{member}: content is {got_len} bytes with SHA-256 {got}, pinned {expected_len} bytes with {expected}")]
    Content {
        member: String,
        got_len: u64,
        got: String,
        expected_len: u64,
        expected: String,
    },
    #[error("the OS image is {found:?}, the catalog says {declared:?}")]
    Container {
        declared: Container,
        found: Option<Container>,
    },
    #[error("{0} has no container pins this reads")]
    NoContainer(String),
    #[error(
        "{member}: the container is {got_len} bytes with SHA-256 {got}, pinned {expected_len} bytes with {expected}"
    )]
    ContainerContent {
        member: String,
        got_len: u64,
        got: String,
        expected_len: u64,
        expected: String,
    },
    #[error("UDIF: {0}")]
    Udif(#[from] udif::DppError),
    #[error("UDIF: {0}")]
    Layout(String),
    #[error("writing: {0}")]
    Write(#[source] io::Error),
    #[error(transparent)]
    Io(#[from] io::Error),
}
