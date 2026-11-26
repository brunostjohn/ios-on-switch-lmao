// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected kind's boot case (install/boot/defs.bzl boot_case): the records, the boot assets, the IPSW's members
//! and //boot:package's tree.
#![allow(dead_code)]

use std::collections::BTreeMap;
use std::fs::File;
use std::io::BufReader;
use std::path::{Path, PathBuf};

use serde_json::Value;
use switch_boot::{Assets, LaunchdCatalog, Members, Record};

pub fn read(path: &Path) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

pub struct Case {
    json: Value,
    pub kind: String,
}

/// The IPSW's files the case names, owned.
pub struct MemberFiles(BTreeMap<String, Vec<u8>>);

impl MemberFiles {
    pub fn get_mut(&mut self, name: &str) -> &mut Vec<u8> {
        self.0.get_mut(name).unwrap_or_else(|| panic!("the case has no {name}"))
    }

    pub fn remove(&mut self, name: &str) -> Option<Vec<u8>> {
        self.0.remove(name)
    }

    pub fn insert(&mut self, name: &str, data: Vec<u8>) {
        self.0.insert(name.to_owned(), data);
    }

    pub fn members(&self) -> Members<'_> {
        let file = |name: &str| {
            self.0
                .get(name)
                .unwrap_or_else(|| panic!("the case has no {name}"))
                .as_slice()
        };
        let catalog = match (self.0.get("launchd_catalog"), self.0.get("launchd_catalog_signature")) {
            (Some(plist), Some(signature)) => Some(LaunchdCatalog { plist, signature }),
            _ => None,
        };
        Members {
            kernelcache: file("kernelcache"),
            device_tree: file("device_tree"),
            static_trust_cache: file("static_trust_cache"),
            system_cryptex_trust_cache: file("system_cryptex_trust_cache"),
            app_cryptex_trust_cache: file("app_cryptex_trust_cache"),
            installd: file("installd"),
            system_version: file("system_version"),
            launchd_catalog: catalog,
        }
    }
}

impl Case {
    pub fn load() -> Self {
        let path = std::env::var("SWITCH_BOOT_CASE").expect("SWITCH_BOOT_CASE is set by the BUILD file");
        let json: Value = serde_json::from_slice(&read(Path::new(&path))).unwrap();
        let kind = json["kind"].as_str().unwrap().to_owned();
        Self { json, kind }
    }

    fn path(&self, key: &str) -> PathBuf {
        PathBuf::from(
            self.json[key]
                .as_str()
                .unwrap_or_else(|| panic!("the case has no {key}")),
        )
    }

    pub fn record(&self, kind: &str) -> Record {
        let json = serde_json::to_vec(&self.json["records"][kind]).unwrap();
        Record::from_json(&json).unwrap()
    }

    pub fn other_kinds(&self) -> Vec<String> {
        let records = self.json["records"].as_object().unwrap();
        records.keys().filter(|k| **k != self.kind).cloned().collect()
    }

    pub fn assets(&self) -> Assets {
        let tar = File::open(self.path("assets")).unwrap();
        Assets::from_tar(BufReader::new(tar)).unwrap()
    }

    pub fn member_files(&self) -> MemberFiles {
        let members = self.json["members"].as_object().unwrap();
        MemberFiles(
            members
                .iter()
                .map(|(name, path)| (name.clone(), read(Path::new(path.as_str().unwrap()))))
                .collect(),
        )
    }

    pub fn package(&self) -> PathBuf {
        self.path("package")
    }

    /// Every file of //boot:package's tree, by its path in it.
    pub fn package_paths(&self) -> Vec<String> {
        fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
            for entry in std::fs::read_dir(dir).unwrap() {
                let path = entry.unwrap().path();
                if std::fs::metadata(&path).unwrap().is_dir() {
                    walk(root, &path, out);
                } else {
                    out.push(path.strip_prefix(root).unwrap().to_str().unwrap().to_owned());
                }
            }
        }
        let mut out = Vec::new();
        walk(&self.package(), &self.package(), &mut out);
        out.sort();
        out
    }
}
