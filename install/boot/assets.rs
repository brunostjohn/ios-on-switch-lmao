// SPDX-License-Identifier: GPL-3.0-or-later
//! The release's boot assets for one build (`//boot:boot_assets`, one tar): every file the boot steps take besides
//! the IPSW's, by member name. `tree/` holds the files the boot entry carries as they are, at their paths in it.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Component, Path};

use serde::Deserialize;

use crate::Error;
use crate::record::{Prelink, Record};

pub const STAMP: &str = "boot.json";
const FORMAT: u32 = 1;

pub const TREE: &str = "tree/";
pub const SITES: &str = "kernel/sites/";
pub const KERNEL: &str = "kernel/kernel.macho";
pub const PRELINK_OPTIONS: &str = "kernel/prelink.json";
pub const PRELINK_PREVIOUS: &str = "kernel/dma-prelink.json";
pub const PRELINK_INHERITED: &str = "kernel/dma-kernel.json";
pub const BINDINGS: &str = "kernel/prelink-bindings.json";
pub const FILESET_PINS: &str = "kernel/prelink-pins.json";
pub const ADT_BASE: &str = "adt/ios-base.adt";
pub const OVERLAY_TRUST: &str = "trust/overlay-base.trustcache";
pub const GPU_TRUST: &str = "trust/gpu.trustcache";
pub const PRETRANSLATED_OS: &str = "pretranslated/os.trustcache";
pub const PRETRANSLATED_SYSTEM_CRYPTEX: &str = "pretranslated/system_cryptex.trustcache";
pub const PRETRANSLATED_APP_CRYPTEX: &str = "pretranslated/app_cryptex.trustcache";
pub const PRETRANSLATED_CACHE_LIST: &str = "pretranslated/a64x-cache-pretranslated.bin";
pub const BOOTSTRAP_JOB: &str = "launchd/bootstrap.plist";
pub const APPS_JOB: &str = "launchd/bootstrap-apps.plist";
pub const SDWAKE_JOB: &str = "launchd/sdwake.plist";
pub const BOOT_ARGS: &str = "window/boot-args.txt";
pub const DATA_SEED: &str = "window/data-seed.dbr";
pub const U_BOOT: &str = "bl33/u-boot.bin";
pub const SCRIPT: &str = "bl33/boot-script.img";
pub const LOADER: &str = "bl33/loader.uboot.bin";
pub const LOADER_CONSTANTS: &str = "bl33/constants.json";
pub const EXPORTER: &str = "bl33/exporter.uimg";
pub const DTB: &str = "bl33/oled-sku3.dtb";

const ALWAYS: [&str; 12] = [
    STAMP,
    KERNEL,
    ADT_BASE,
    OVERLAY_TRUST,
    BOOT_ARGS,
    DATA_SEED,
    U_BOOT,
    SCRIPT,
    LOADER,
    LOADER_CONSTANTS,
    EXPORTER,
    DTB,
];
const OPTIONAL: [&str; 5] = [
    GPU_TRUST,
    PRETRANSLATED_OS,
    PRETRANSLATED_SYSTEM_CRYPTEX,
    PRETRANSLATED_APP_CRYPTEX,
    PRETRANSLATED_CACHE_LIST,
];
const NATIVE: [&str; 3] = [PRELINK_OPTIONS, PRELINK_PREVIOUS, PRELINK_INHERITED];
const FILESET: [&str; 2] = [BINDINGS, FILESET_PINS];
const LAUNCHD: [&str; 3] = [BOOTSTRAP_JOB, APPS_JOB, SDWAKE_JOB];

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Stamp {
    format: u32,
    build: String,
}

/// A bundle's members, read whole: the kernel is the largest at under 100 MB.
pub struct Assets {
    files: BTreeMap<String, Vec<u8>>,
}

fn member_name(path: &Path) -> Result<String, Error> {
    let mut parts = Vec::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(part) => parts.push(part.to_str().ok_or_else(|| bad(path, "a name that is not UTF-8"))?),
            _ => return Err(bad(path, "a path outside the bundle")),
        }
    }
    if parts.is_empty() {
        return Err(bad(path, "an empty name"));
    }
    Ok(parts.join("/"))
}

fn bad(path: &Path, what: &str) -> Error {
    Error::Assets(format!("{}: {what}", path.display()))
}

impl Assets {
    /// The tar's regular files by name. Directories are skipped; links and anything else are refused.
    pub fn from_tar(reader: impl Read) -> Result<Self, Error> {
        let io = |e: std::io::Error| Error::Assets(e.to_string());
        let mut files = BTreeMap::new();
        let mut archive = tar::Archive::new(reader);
        for entry in archive.entries().map_err(io)? {
            let mut entry = entry.map_err(io)?;
            let path = entry.path().map_err(io)?.into_owned();
            match entry.header().entry_type() {
                tar::EntryType::Directory => continue,
                tar::EntryType::Regular => {}
                other => return Err(bad(&path, &format!("a {other:?} entry, not a file"))),
            }
            let name = member_name(&path)?;
            let mut data = Vec::with_capacity(entry.size() as usize);
            entry.read_to_end(&mut data).map_err(io)?;
            if files.insert(name, data).is_some() {
                return Err(bad(&path, "a second member of that name"));
            }
        }
        Ok(Self { files })
    }

    pub fn get(&self, name: &str) -> Option<&[u8]> {
        self.files.get(name).map(Vec::as_slice)
    }

    /// Replaces or adds a member, returning the old one.
    pub fn insert(&mut self, name: &str, data: Vec<u8>) -> Option<Vec<u8>> {
        self.files.insert(name.to_owned(), data)
    }

    pub fn remove(&mut self, name: &str) -> Option<Vec<u8>> {
        self.files.remove(name)
    }

    pub(crate) fn file(&self, name: &str) -> Result<&[u8], Error> {
        self.get(name).ok_or_else(|| Error::Assets(format!("no {name}")))
    }

    pub(crate) fn under<'a>(&'a self, prefix: &'a str) -> impl Iterator<Item = (&'a str, &'a [u8])> + 'a {
        self.files
            .range(prefix.to_owned()..)
            .take_while(move |(name, _)| name.starts_with(prefix))
            .map(move |(name, data)| (&name[prefix.len()..], data.as_slice()))
    }

    /// The bundle is this build's, in the format this reads, and holds exactly what the record's steps take.
    pub(crate) fn check(&self, record: &Record) -> Result<(), Error> {
        let stamp: Stamp =
            serde_json::from_slice(self.file(STAMP)?).map_err(|e| Error::Assets(format!("{STAMP}: {e}")))?;
        if stamp.format != FORMAT {
            return Err(Error::Assets(format!(
                "format {}, this reads format {FORMAT}",
                stamp.format
            )));
        }
        if stamp.build != record.build {
            return Err(Error::AssetsForAnotherBuild {
                assets: stamp.build,
                record: record.build.clone(),
            });
        }
        let prelink: &[&str] = match record.prelink {
            Prelink::Native => &NATIVE,
            Prelink::Fileset => &FILESET,
        };
        let launchd: &[&str] = if record.launchd_catalog.is_some() {
            &LAUNCHD
        } else {
            &[]
        };
        let required: Vec<&str> = ALWAYS.iter().chain(prelink).chain(launchd).copied().collect();
        if let Some(missing) = required.iter().find(|name| !self.files.contains_key(**name)) {
            return Err(Error::Assets(format!("no {missing}")));
        }
        let known = |name: &str| {
            required.contains(&name) || OPTIONAL.contains(&name) || name.starts_with(TREE) || name.starts_with(SITES)
        };
        match self.files.keys().find(|name| !known(name)) {
            Some(unknown) => Err(Error::Assets(format!(
                "{unknown} is no member of a {} bundle",
                record.build
            ))),
            None => Ok(()),
        }
    }
}
