// SPDX-License-Identifier: GPL-3.0-or-later
//! Apple Encrypted Archives with a symmetric key (profile 1), as the IPSW's OS and SystemOS members are.

mod index;
mod key;
mod pipeline;
mod segment;
#[cfg(test)]
mod tests;
mod wkms;

pub use index::{Index, Segment};
pub use key::key_value;
pub use pipeline::{decrypt, decrypt_member, decrypt_member_with_key_fetch};
pub use wkms::{
    AuthData, fetch_fcs_key_pem, parse_auth_fields, read_auth_data, unwrap_key, unwrap_member_key,
    unwrap_member_key_with, validate_fcs_url,
};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("not a 32-byte key in base64: or hex: form")]
    Key,
    #[error("reading the archive: {0}")]
    Read(std::io::Error),
    #[error("writing the output: {0}")]
    Write(std::io::Error),
    #[error("unsupported archive: {0}")]
    Unsupported(String),
    #[error("malformed archive: {0}")]
    Malformed(String),
    #[error("AEA auth data: {0}")]
    AuthData(String),
    #[error("fetching Apple decryption key from {url}: {reason}")]
    KeyFetch { url: String, reason: String },
    #[error("unwrapping Apple decryption key: {0}")]
    KeyUnwrap(String),
    #[error("{0} fails its HMAC: a wrong key, or a damaged archive")]
    Authentication(String),
    #[error("segment {cluster}/{index}: {reason}")]
    Segment { cluster: u32, index: u32, reason: String },
    #[error("{0} is no encrypted archive")]
    NotEncrypted(String),
    #[error("the archive decrypts to {archive} bytes, and the catalog pins {pinned}")]
    RawSize { archive: u64, pinned: u64 },
    #[error("the output is {got} bytes, not the archive's {expected}")]
    Length { got: u64, expected: u64 },
    #[error("the output's SHA-256 is {got}, not the pinned {expected}")]
    Content { got: String, expected: String },
}

pub type Result<T> = std::result::Result<T, Error>;
