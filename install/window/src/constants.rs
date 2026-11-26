// SPDX-License-Identifier: GPL-3.0-or-later
//! The loader's constants (build_native_loader.py window_checks, staged_window and join): everything the release's
//! loader was compiled for. constants.json is written as `json.dumps(constants | {'BLOCKS': table}, indent=1)` writes
//! it, and a loader built for one window refuses any other, as bl33.py check_loader_constants refuses it.

use serde_json::{Map, Value};

use crate::Error;

/// The constants in window_checks' order: constants.json keeps it.
pub const NAMES: [&str; 31] = [
    "SOURCE",
    "DEST",
    "BYTES",
    "PACKED_BYTES",
    "STREAM_BYTES",
    "STAMPS_ADT_OFFSET",
    "SEED_ADT_OFFSET",
    "SEED_SOURCE_ADT_OFFSET",
    "STAGE_ID",
    "EMC_KHZ",
    "ARGS",
    "ENTRY",
    "CRC",
    "VERIFY_KERNEL",
    "VIRT",
    "ADT_PA",
    "BOOT_PHYS",
    "BOOT_VIRT",
    "BOOT_SIZE",
    "BOOT_ACTUAL",
    "FULL_RAM",
    "RAM_ADT_OFFSET",
    "WINDOW",
    "TOP",
    "ADT_VA",
    "ADT_BYTES",
    "NATIVE_GATES",
    "SD_REFERENCE",
    "SD_CARD",
    "SD_REFERENCE_ADT_OFFSET",
    "FAST_COPY",
];

/// The staged sizes and the stage id: unknown until the window is compressed.
pub const STAGED: [&str; 3] = ["PACKED_BYTES", "STREAM_BYTES", "STAGE_ID"];

/// A block of the staged window: window offset, length, offset in the stream, stream bytes | STORED.
pub type Row = [u64; 4];
pub const STORED: u64 = 1 << 63;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Constants {
    values: [Option<u64>; NAMES.len()],
    pub blocks: Vec<Row>,
}

fn index(name: &str) -> usize {
    NAMES
        .iter()
        .position(|n| *n == name)
        .unwrap_or_else(|| panic!("no loader constant {name}"))
}

impl Constants {
    /// Every constant but the staged ones, which stay unknown.
    pub fn new(known: &[(&str, u64)]) -> Self {
        let mut values = [None; NAMES.len()];
        for (name, value) in known {
            values[index(name)] = Some(*value);
        }
        let missing: Vec<&str> = NAMES
            .iter()
            .zip(&values)
            .filter(|(n, v)| v.is_none() && !STAGED.contains(n))
            .map(|(n, _)| *n)
            .collect();
        assert!(missing.is_empty(), "loader constants without a value: {missing:?}");
        Self {
            values,
            blocks: Vec::new(),
        }
    }

    pub fn get(&self, name: &str) -> Option<u64> {
        self.values[index(name)]
    }

    pub fn set(&mut self, name: &str, value: u64) {
        self.values[index(name)] = Some(value);
    }

    pub fn stage_id(&self) -> Option<u64> {
        self.get("STAGE_ID")
    }

    fn value(&self) -> Value {
        let mut map = Map::new();
        for (name, value) in NAMES.iter().zip(&self.values) {
            map.insert((*name).into(), value.map_or(Value::Null, Value::from));
        }
        map.insert(
            "BLOCKS".into(),
            self.blocks.iter().map(|row| Value::from(row.to_vec())).collect(),
        );
        Value::Object(map)
    }

    /// constants.json, byte for byte.
    pub fn json(&self) -> String {
        let mut out = String::from("{\n");
        for (name, value) in NAMES.iter().zip(&self.values) {
            let value = value.map_or_else(|| "null".to_owned(), |v| v.to_string());
            out += &format!(" \"{name}\": {value},\n");
        }
        if self.blocks.is_empty() {
            out += " \"BLOCKS\": []\n}\n";
            return out;
        }
        out += " \"BLOCKS\": [\n";
        let rows: Vec<String> = self
            .blocks
            .iter()
            .map(|row| format!("  [\n{}\n  ]", row.map(|v| format!("   {v}")).join(",\n")))
            .collect();
        out += &rows.join(",\n");
        out += "\n ]\n}\n";
        out
    }

    /// Refuse unless `built_for`, a constants.json, holds exactly these constants.
    pub fn check_against(&self, built_for: &[u8]) -> Result<(), Error> {
        let built_for: Value = serde_json::from_slice(built_for).map_err(|e| Error::Constants(e.to_string()))?;
        let (Value::Object(theirs), Value::Object(ours)) = (built_for, self.value()) else {
            return Err(Error::Constants("not a JSON object".into()));
        };
        let mut differ: Vec<String> = theirs
            .keys()
            .chain(ours.keys())
            .filter(|k| theirs.get(*k) != ours.get(*k))
            .cloned()
            .collect();
        differ.sort();
        differ.dedup();
        if differ.is_empty() {
            Ok(())
        } else {
            Err(Error::LoaderBuiltForAnotherWindow(differ))
        }
    }
}
