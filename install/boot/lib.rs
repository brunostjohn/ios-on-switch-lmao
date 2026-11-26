// SPDX-License-Identifier: GPL-3.0-or-later
//! The boot entry `//boot:package` makes, made on the user's computer from the release's boot assets for a build and
//! the IPSW's members: `switchroot/`, `bootloader/`, and the overlay files made from Apple's (`installd`, and the
//! launchd catalogue where the build has one). Every file is CI's byte for byte, or the release's loader refuses
//! the kernel window.
//!
//! The steps, as boot/defs.bzl orders them: prelink, the launchd catalogue, `installd`, the ADT's finish, the trust
//! bundle, the kernel window, `bl33.bin`. Which of them run, and on what, is the build's catalog record and the
//! bundle's files.

mod assets;
mod steps;

#[cfg(test)]
mod tests;

use std::collections::BTreeMap;
use std::path::Path;

pub use assets::{Assets, PRETRANSLATED_EDITS};
use switch_catalog::Build;

/// Where Apple's launchd catalogue is on the OS volume, and where the overlay puts ours.
pub const LAUNCHD_CATALOG: &str = "/System/Library/xpc/launchd.plist";

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("the boot assets are {assets}'s, not {record}'s")]
    AssetsForAnotherBuild { assets: String, record: String },
    #[error("the boot assets: {0}")]
    Assets(String),
    #[error(transparent)]
    Catalog(#[from] switch_catalog::Error),
    #[error("the IPSW's files: {0}")]
    Members(&'static str),
    #[error("IM4P: {0}")]
    Img4(#[from] switch_img4::Error),
    #[error("prelink: {0}")]
    Prelink(#[from] switch_prelink::Error),
    #[error("launchd catalogue: {0}")]
    Launchd(#[from] switch_launchd::Error),
    #[error(transparent)]
    Installd(#[from] switch_installd::Error),
    #[error("ADT: {0}")]
    Adt(#[from] switch_adt::Error),
    #[error(transparent)]
    Window(#[from] switch_window::Error),
}

/// Apple's catalogue and its detached signature, from the OS volume.
pub struct LaunchdCatalog<'a> {
    pub plist: &'a [u8],
    pub signature: &'a [u8],
}

/// What the IPSW gives: its IM4P members as inflated from the zip, and files of its OS volume.
pub struct Members<'a> {
    pub kernelcache: &'a [u8],
    pub device_tree: &'a [u8],
    pub static_trust_cache: &'a [u8],
    pub system_cryptex_trust_cache: &'a [u8],
    pub app_cryptex_trust_cache: &'a [u8],
    /// `/usr/libexec/installd`.
    pub installd: &'a [u8],
    /// `/System/Library/CoreServices/SystemVersion.plist`.
    pub system_version: &'a [u8],
    /// Given exactly when the build runs the launchd_catalog step.
    pub launchd_catalog: Option<LaunchdCatalog<'a>>,
}

/// The boot entry's files by their path in `//boot:package`'s tree.
pub struct Package {
    pub files: BTreeMap<String, Vec<u8>>,
    pub installd_cdhash: [u8; 20],
}

impl Package {
    pub fn write(&self, root: &Path) -> std::io::Result<()> {
        for (path, data) in &self.files {
            let target = root.join(path);
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent)?;
            }
            std::fs::write(target, data)?;
        }
        Ok(())
    }
}

fn overlay(path: &str) -> String {
    format!("overlay{path}")
}

/// The boot entry for `build`. The prelink, the longest step, runs beside the others until the window needs the
/// kernel.
pub fn boot_files(build: &Build, assets: &Assets, members: &Members) -> Result<Package, Error> {
    assets.check(build)?;
    let (kernel, rest) = rayon::join(
        || steps::prelink(build, assets, members.kernelcache),
        || -> Result<_, Error> {
            let apple = steps::apple(build, members)?;
            let launchd = steps::launchd(build, assets, members.launchd_catalog.as_ref())?;
            let installd = switch_installd::prepare(members.installd, members.system_version, build.build())?;
            let adt = steps::adt(build, assets, &apple, launchd.as_ref())?;
            let trust = steps::trust(assets, &apple, launchd.as_ref(), installd.cdhash)?;
            Ok((launchd, installd, adt, trust))
        },
    );
    let (launchd, installd, adt, trust) = rest?;
    let bl33 = steps::bl33(build, assets, &kernel?, &adt, &trust)?;

    let mut files: BTreeMap<String, Vec<u8>> = assets
        .under(assets::TREE)
        .map(|(path, data)| (path.to_owned(), data.to_vec()))
        .collect();
    files.insert("switchroot/ios/bl33.bin".into(), bl33);
    files.insert(overlay(switch_installd::PATH), installd.bytes);
    if let Some(catalog) = launchd {
        files.insert(overlay(LAUNCHD_CATALOG), catalog.plist);
        files.insert(overlay(&format!("{LAUNCHD_CATALOG}.sig")), catalog.signature);
    }
    Ok(Package {
        files,
        installd_cdhash: installd.cdhash,
    })
}
