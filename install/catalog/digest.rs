// SPDX-License-Identifier: GPL-3.0-or-later
use std::fmt;

use serde::de::Error as _;
use serde::{Deserialize, Deserializer};

/// A hash as the catalog writes it, in lower-case hex.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Digest<const N: usize>(pub [u8; N]);

pub type Sha256 = Digest<32>;
pub type Sha384 = Digest<48>;

impl<const N: usize> Digest<N> {
    pub fn from_hex(text: &str) -> Option<Self> {
        let mut out = [0; N];
        hex::decode_to_slice(text, &mut out).ok()?;
        Some(Self(out))
    }
}

impl<const N: usize> fmt::Display for Digest<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&hex::encode(self.0))
    }
}

impl<const N: usize> fmt::Debug for Digest<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, f)
    }
}

impl<'de, const N: usize> Deserialize<'de> for Digest<N> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let text = String::deserialize(deserializer)?;
        Self::from_hex(&text).ok_or_else(|| D::Error::custom(format!("{text:?} is not {N} bytes of hex")))
    }
}

pub(crate) fn hex_u64<'de, D: Deserializer<'de>>(deserializer: D) -> Result<u64, D::Error> {
    let text = String::deserialize(deserializer)?;
    text.strip_prefix("0x")
        .and_then(|hex| u64::from_str_radix(hex, 16).ok())
        .ok_or_else(|| D::Error::custom(format!("{text:?} is not 0x-hex")))
}
