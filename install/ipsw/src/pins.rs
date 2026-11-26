// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use serde::{Deserialize, Deserializer, de::Error as _};
use switch_fetch::{Span, Url};

/// One kind of ipsw/pins.bzl's IPSWS, as far as reading its members goes. A declared but unpinned kind has no members.
#[derive(Clone, Debug, Deserialize)]
pub struct Ipsw {
    #[serde(default)]
    pub build: String,
    #[serde(default)]
    pub url: String,
    #[serde(default)]
    pub members: BTreeMap<String, Member>,
}

/// A member's pins: where its stored bytes sit in the IPSW, their SHA-256, and its content's size and SHA-256.
#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
pub struct Member {
    pub member: String,
    pub offset: u64,
    pub stored_size: u64,
    pub size: u64,
    #[serde(deserialize_with = "hex32")]
    pub sha256: [u8; 32],
    #[serde(deserialize_with = "hex32")]
    pub content_sha256: [u8; 32],
    pub deflated: bool,
    #[serde(default)]
    pub aea_key: Option<String>,
}

impl Member {
    pub fn file_name(&self) -> &str {
        self.member.rsplit('/').next().unwrap_or(&self.member)
    }

    pub fn span(&self, url: &Url) -> Span {
        Span {
            url: url.clone(),
            offset: self.offset,
            len: self.stored_size,
            sha256: self.sha256,
        }
    }
}

pub fn parse_pins(json: &str) -> serde_json::Result<BTreeMap<String, Ipsw>> {
    serde_json::from_str(json)
}

fn hex32<'de, D: Deserializer<'de>>(deserializer: D) -> Result<[u8; 32], D::Error> {
    let text = String::deserialize(deserializer)?;
    let mut out = [0u8; 32];
    hex::decode_to_slice(&text, &mut out).map_err(D::Error::custom)?;
    Ok(out)
}
