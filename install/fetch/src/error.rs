// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;
use std::path::{Path, PathBuf};

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{url}: {source}")]
    Http {
        url: String,
        #[source]
        source: reqwest::Error,
    },
    #[error("{url}: HTTP {status} for bytes {first}-{last}, where 206 Partial Content was expected")]
    NotPartial {
        url: String,
        status: u16,
        first: u64,
        last: u64,
    },
    #[error("{url}: Content-Range {got:?} for bytes {first}-{last}")]
    ContentRange {
        url: String,
        got: Option<String>,
        first: u64,
        last: u64,
    },
    #[error("{url}: {source}")]
    Body {
        url: String,
        #[source]
        source: io::Error,
    },
    #[error("{url}: the body is {got} bytes where {expected} were asked for")]
    BodyLength { url: String, got: u64, expected: u64 },
    #[error("{}: SHA-256 {got}, expected {expected}", .path.display())]
    HashMismatch {
        path: PathBuf,
        got: String,
        expected: String,
    },
    #[error("{}: {source}", .path.display())]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("SHA256SUMS line {line}: {text:?}")]
    Sums { line: usize, text: String },
    #[error("{name}: SHA256SUMS lists {listed:?}, the catalog pins {pinned}")]
    Release {
        name: String,
        listed: Option<String>,
        pinned: String,
    },
    #[error("{base}: no URL for {name:?}")]
    Url { base: String, name: String },
}

pub(crate) fn http(url: &reqwest::Url) -> impl FnOnce(reqwest::Error) -> Error + '_ {
    move |source| Error::Http {
        url: url.to_string(),
        source,
    }
}

pub(crate) fn io_at(path: &Path) -> impl FnOnce(io::Error) -> Error + '_ {
    move |source| Error::Io {
        path: path.to_owned(),
        source,
    }
}
