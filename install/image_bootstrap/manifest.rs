// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeSet;

use serde_json::{Map, Value};

use crate::Error;

pub const DIRECTORY: &str = "Switch/bootstrap";
pub const MANIFEST: &str = "manifest.json";
pub const ARCHIVE: &str = "payload.tar";
pub const HELPER: &str = "switch-bootstrap";
pub const ROOT: &str = "root";
pub const METADATA: &str = "metadata.json";
pub const MIGRATION_PENDING: &str = "migration.pending";
pub const HELD: [&str; 4] = ["org.coolstar.sileo", "ellekit", "uikittools", "launchctl"];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootstrapPins<'a> {
    pub build: &'a str,
    pub version: &'a str,
}

impl BootstrapPins<'_> {
    pub fn validate(&self) -> Result<(), Error> {
        if !is_build_id(self.build) || !is_dotted_version(self.version) {
            return Err(Error::Manifest("invalid IPSW build or firmware version".into()));
        }
        Ok(())
    }
}

pub fn is_build_id(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric())
}

pub fn is_dotted_version(s: &str) -> bool {
    !s.is_empty()
        && s.split('.')
            .all(|part| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit()))
}

pub fn is_sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

#[derive(Clone, Debug)]
pub struct BootstrapManifest {
    raw: Map<String, Value>,
    archive_sha256: String,
    helper_sha256: String,
    managed_generation: Option<String>,
}

impl BootstrapManifest {
    pub fn raw(&self) -> &Map<String, Value> {
        &self.raw
    }

    pub fn archive_sha256(&self) -> &str {
        &self.archive_sha256
    }

    pub fn helper_sha256(&self) -> &str {
        &self.helper_sha256
    }

    pub fn managed_generation(&self) -> Option<&str> {
        self.managed_generation.as_deref()
    }

    pub fn with_installed_pins(&self, build: &str, version: &str) -> Value {
        let mut out = self.raw.clone();
        out.insert("build".into(), Value::String(build.to_owned()));
        out.insert("firmware".into(), Value::String(version.to_owned()));
        Value::Object(out)
    }
}

pub fn parse_manifest(bytes: &[u8], pins: Option<&BootstrapPins<'_>>) -> Result<BootstrapManifest, Error> {
    let value: Value =
        serde_json::from_slice(bytes).map_err(|e| Error::Manifest(format!("invalid manifest JSON: {e}")))?;
    let Value::Object(map) = value else {
        return Err(Error::Manifest("manifest must be a JSON object".into()));
    };

    if map.get("schema_version").and_then(Value::as_u64) != Some(1) {
        return Err(Error::Manifest("invalid manifest schema_version".into()));
    }

    let archive_sha256 = map
        .get("archive_sha256")
        .and_then(Value::as_str)
        .filter(|s| is_sha256_hex(s))
        .ok_or_else(|| Error::Manifest("invalid manifest archive_sha256".into()))?
        .to_owned();

    let helper_sha256 = map
        .get("helper_sha256")
        .and_then(Value::as_str)
        .filter(|s| is_sha256_hex(s))
        .ok_or_else(|| Error::Manifest("invalid manifest helper_sha256".into()))?
        .to_owned();

    let holds_arr = map
        .get("holds")
        .and_then(Value::as_array)
        .ok_or_else(|| Error::Manifest("invalid manifest holds".into()))?;
    let mut holds = BTreeSet::new();
    for item in holds_arr {
        let Some(name) = item.as_str() else {
            return Err(Error::Manifest("invalid manifest holds entry".into()));
        };
        holds.insert(name);
    }
    let expected: BTreeSet<&str> = HELD.into_iter().collect();
    if holds != expected {
        return Err(Error::Manifest(
            "manifest holds differ from required core packages".into(),
        ));
    }

    let managed_generation = match map.get("managed_generation") {
        None => None,
        Some(Value::String(s)) if is_sha256_hex(s) => Some(s.clone()),
        Some(_) => {
            return Err(Error::Manifest("invalid managed package generation".into()));
        }
    };

    if let Some(pins) = pins {
        pins.validate()?;
        if let Some(v) = map.get("build")
            && v.as_str() != Some(pins.build)
        {
            return Err(Error::Manifest("build differs from selected IPSW pins".into()));
        }
        if let Some(v) = map.get("firmware")
            && v.as_str() != Some(pins.version)
        {
            return Err(Error::Manifest("firmware differs from selected IPSW pins".into()));
        }
    }

    Ok(BootstrapManifest {
        raw: map,
        archive_sha256,
        helper_sha256,
        managed_generation,
    })
}
