// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;
use std::time::Duration;

use crate::policy::Refusal;

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{path} can't be written: {reason}")]
    Refused { path: String, reason: Refusal },
    #[error("{0} was removed or replaced since it was listed")]
    Changed(String),
    #[error("{volume} is in use and can't be unmounted: {detail}")]
    Busy { volume: String, detail: String },
    #[error("writing a whole disk on Windows needs an administrator: run the installer as administrator")]
    NotElevated,
    #[error("access to {path} was not authorized: {detail}")]
    NotAuthorized { path: String, detail: String },
    #[error("{op} of {len} bytes at {offset} is past the end of the disk ({size} bytes)")]
    OutOfRange {
        op: &'static str,
        offset: u64,
        len: u64,
        size: u64,
    },
    #[error("timed out after {waited:?} waiting for {what}")]
    Timeout { what: String, waited: Duration },
    #[error("{what}: {detail}")]
    Os { what: String, detail: String },
    #[error("{what}: {source}")]
    Io {
        what: String,
        #[source]
        source: io::Error,
    },
}

impl Error {
    pub(crate) fn io(what: impl Into<String>, source: io::Error) -> Self {
        Error::Io {
            what: what.into(),
            source,
        }
    }

    #[cfg_attr(windows, allow(dead_code))]
    pub(crate) fn os(what: impl Into<String>, detail: impl Into<String>) -> Self {
        Error::Os {
            what: what.into(),
            detail: detail.into(),
        }
    }
}
