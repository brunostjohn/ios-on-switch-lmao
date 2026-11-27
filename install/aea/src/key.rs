// SPDX-License-Identifier: GPL-3.0-or-later
use base64::Engine;
use base64::engine::general_purpose::STANDARD;

use crate::{Error, Result};

/// A symmetric key as `aea -key-value` takes it and the IPSW pins store it: `base64:` or `hex:`, then 32 bytes.
pub fn key_value(text: &str) -> Result<[u8; 32]> {
    let bytes = if let Some(encoded) = text.strip_prefix("base64:") {
        STANDARD.decode(encoded).ok()
    } else if let Some(encoded) = text.strip_prefix("hex:") {
        hex::decode(encoded).ok()
    } else {
        None
    };
    bytes.and_then(|bytes| bytes.try_into().ok()).ok_or(Error::Key)
}
