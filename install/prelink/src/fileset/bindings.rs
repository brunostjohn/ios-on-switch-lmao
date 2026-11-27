// SPDX-License-Identifier: GPL-3.0-or-later
//! `prelink-bindings.json`, format 1 (`translate/arm64e/xnu/prelink26/kernel_bindings.py`): which kexts CI prelinked,
//! with which slide, and each Apple kernel address they use bound to ours. It holds for one kernelcache and one
//! unprelinked kernel, named by their SHA-256.

use std::collections::{BTreeMap, HashMap};

use serde::Deserialize;

use crate::error::{Result, json, refused};
use crate::pyint;

const FORMAT: i64 = 1;

pub(crate) struct Cut {
    pub component: String,
    pub dependency: String,
    pub sites: i64,
}

pub(crate) struct Bindings {
    pub build: String,
    kernelcache_sha256: String,
    kernel_sha256: String,
    pub slide: Option<i64>,
    pub roots: Vec<String>,
    pub cuts: Vec<Cut>,
    pub deferred: BTreeMap<String, Vec<String>>,
    table: HashMap<u64, u64>,
}

#[derive(Deserialize)]
struct Format {
    format: Option<serde_json::Value>,
}

#[derive(Deserialize)]
struct Export {
    build: String,
    kernelcache_sha256: String,
    kernel_sha256: String,
    slide: Option<i64>,
    roots: Vec<String>,
    cuts: Vec<(String, String, i64)>,
    deferred: BTreeMap<String, Vec<String>>,
    bindings: Vec<Binding>,
}

#[derive(Deserialize)]
struct Binding {
    apple: String,
    ours: String,
}

impl Bindings {
    pub fn read(bytes: &[u8]) -> Result<Self> {
        let format = json::<Format>("prelink-bindings.json", bytes)?.format;
        if format.as_ref().and_then(serde_json::Value::as_i64) != Some(FORMAT) {
            let shown = format.map_or("None".to_owned(), |f| f.to_string());
            return refused(format!(
                "prelink-bindings.json: format {shown}, this prelink reads format {FORMAT}"
            ));
        }
        let export: Export = json("prelink-bindings.json", bytes)?;
        let mut table = HashMap::with_capacity(export.bindings.len());
        for binding in &export.bindings {
            let apple = address(&binding.apple)?;
            if table.insert(apple, address(&binding.ours)?).is_some() {
                return refused(format!("prelink-bindings.json: {apple:#x} bound twice"));
            }
        }
        let mut cuts: Vec<Cut> = Vec::with_capacity(export.cuts.len());
        for (component, dependency, sites) in export.cuts {
            if cuts
                .iter()
                .any(|c| c.component == component && c.dependency == dependency)
            {
                return refused(format!("prelink-bindings.json: cut {component}={dependency} twice"));
            }
            cuts.push(Cut {
                component,
                dependency,
                sites,
            });
        }
        Ok(Bindings {
            build: export.build,
            kernelcache_sha256: export.kernelcache_sha256,
            kernel_sha256: export.kernel_sha256,
            slide: export.slide,
            roots: export.roots,
            cuts,
            deferred: export.deferred,
            table,
        })
    }

    /// Refuses a kernelcache or kernel other than the two the export was resolved against (SHA-256, lowercase hex).
    pub fn check_made_for(&self, kernelcache_sha256: &str, kernel_sha256: &str) -> Result<()> {
        for (key, made_for, given) in [
            ("kernelcache_sha256", &self.kernelcache_sha256, kernelcache_sha256),
            ("kernel_sha256", &self.kernel_sha256, kernel_sha256),
        ] {
            if made_for != given {
                return refused(format!(
                    "prelink-bindings.json: made for the {key} {made_for}, given {given}"
                ));
            }
        }
        Ok(())
    }

    /// Our address bound to Apple's.
    pub fn resolve(&self, apple: u64) -> Option<u64> {
        self.table.get(&apple).copied()
    }

    pub fn is_cut(&self, component: &str, dependency: &str) -> bool {
        self.cuts
            .iter()
            .any(|c| c.component == component && c.dependency == dependency)
    }
}

fn address(text: &str) -> Result<u64> {
    match pyint::parse(text, 16).map(u64::try_from) {
        Some(Ok(value)) => Ok(value),
        _ => refused(format!("prelink-bindings.json: {text:?} is not a 64-bit address")),
    }
}
