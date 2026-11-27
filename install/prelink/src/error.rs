// SPDX-License-Identifier: GPL-3.0-or-later

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// An input the prelink refuses, where `tools/prelink/emit_prelink.py` refuses it.
    #[error("{0}")]
    Refused(String),
    #[error("{what}: {source}")]
    Json {
        what: &'static str,
        source: serde_json::Error,
    },
}

pub(crate) type Result<T> = std::result::Result<T, Error>;

pub(crate) fn refused<T>(message: impl Into<String>) -> Result<T> {
    Err(Error::Refused(message.into()))
}

pub(crate) fn json<T: serde::de::DeserializeOwned>(what: &'static str, bytes: &[u8]) -> Result<T> {
    serde_json::from_slice(bytes).map_err(|source| Error::Json { what, source })
}
