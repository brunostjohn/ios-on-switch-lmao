// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected kind's window case (install/window/defs.bzl window_case): //boot:package's inputs and outputs.

use serde_json::Value;
use switch_trustcache::CDHASH_LEN;
use switch_window::{Boot, Bundle, Inputs, Kind};

pub fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

pub struct Case {
    json: Value,
    pub kind: String,
    pub root_volume_uuid: String,
    pub command_line: String,
    pub modules: Vec<Vec<u8>>,
}

impl Case {
    pub fn load() -> Self {
        let path = std::env::var("SWITCH_WINDOW_CASE").expect("SWITCH_WINDOW_CASE is not set");
        let json: Value = serde_json::from_slice(&read(&path)).unwrap();
        let text = |key: &str| {
            json[key]
                .as_str()
                .unwrap_or_else(|| panic!("case has no {key}"))
                .to_owned()
        };
        let boot_args = String::from_utf8(read(&text("boot_args"))).unwrap();
        let modules = json["modules"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| read(p.as_str().unwrap()))
            .collect();
        Self {
            kind: text("kind"),
            root_volume_uuid: text("root_volume_uuid"),
            command_line: boot_args.trim_end_matches('\n').to_owned(),
            modules,
            json,
        }
    }

    pub fn file(&self, key: &str) -> Vec<u8> {
        read(self.json[key].as_str().unwrap_or_else(|| panic!("case has no {key}")))
    }

    pub fn kind(&self) -> Kind<'_> {
        let virt_base = self.json["virt_base"].as_str().unwrap();
        Kind {
            name: &self.kind,
            virt_base: u64::from_str_radix(virt_base.trim_start_matches("0x"), 16).unwrap(),
            root_volume_uuid: &self.root_volume_uuid,
            boot_manifest_hash: self.json["boot_manifest_hash"].as_bool().unwrap(),
        }
    }

    pub fn installd(&self) -> [u8; CDHASH_LEN] {
        self.file("installd_cdhash").try_into().expect("a 20-byte CDHash")
    }

    pub fn bundle<'a>(&'a self, static_module: &'a [u8], modules: &'a [&'a [u8]]) -> Bundle<'a> {
        Bundle {
            static_module,
            modules,
            installd: self.installd(),
        }
    }

    pub fn inputs<'a>(&'a self, kernel: &'a [u8], adt: &'a [u8], bundle: &'a [u8], seed: &'a [u8]) -> Inputs<'a> {
        Inputs {
            kernel,
            adt,
            trust_bundle: bundle,
            data_seed: seed,
            command_line: &self.command_line,
            kind: self.kind(),
        }
    }
}

/// The release's files bl33.bin is made of, here the package's own, owned.
pub struct BootFiles {
    pub u_boot: Vec<u8>,
    pub script: Vec<u8>,
    pub loader: Vec<u8>,
    pub exporter: Vec<u8>,
    pub dtb: Vec<u8>,
    pub constants: Vec<u8>,
}

impl BootFiles {
    pub fn of(case: &Case) -> Self {
        Self {
            u_boot: case.file("u_boot"),
            script: case.file("script"),
            loader: case.file("loader"),
            exporter: case.file("exporter"),
            dtb: case.file("dtb"),
            constants: case.file("constants"),
        }
    }

    pub fn boot<'a>(&'a self, loader_constants: &'a [u8]) -> Boot<'a> {
        Boot {
            u_boot: &self.u_boot,
            script: &self.script,
            loader: &self.loader,
            exporter: &self.exporter,
            dtb: &self.dtb,
            loader_constants,
        }
    }
}

pub fn slices(modules: &[Vec<u8>]) -> Vec<&[u8]> {
    modules.iter().map(Vec::as_slice).collect()
}
