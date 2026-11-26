// SPDX-License-Identifier: GPL-3.0-or-later
//! Overlay placement (`Switch/overlay`), staged system apps (`staged_system_apps`) and shader-seed tree merging on a
//! `switch_apfs_write::Tree`.

pub mod cms;
pub mod merge;
pub mod overlay;
pub mod seed_manifest;
pub mod source;
pub mod staged_apps;
mod tree_inspect;

pub use cms::{extract_signed_data, verify_embedded_profile};
pub use merge::{MergeReport, merge_tree, merge_tree_at};
pub use overlay::{OverlayReport, place_overlay, tree_digest};
pub use seed_manifest::{
    APP as INSTALLER_APP, BUILTIN_MANIFEST, IDENTIFIER as INSTALLER_IDENTIFIER, MANIFEST as SEED_MANIFEST,
    SeedManifest, parse_seed_manifest,
};
pub use source::{SourceEntry, SourceKind, TreeSource};
pub use staged_apps::{
    INSTALLED, MOBILE_GID, MOBILE_UID, PLACEHOLDERS, SEEDS, SIGNATURES, STAGED, STATES, StagedApp, StagedAppsReport,
    stage_apps, stage_apps_from_source,
};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Write(#[from] switch_apfs_write::WriteError),
    #[error("overlay source: {0}")]
    Source(String),
    #[error("built-in Installer pin: {0}")]
    BuiltinPin(String),
    #[error("Installer seed pin: {0}")]
    SeedPin(String),
    #[error("staged app: {0}")]
    StagedApp(String),
    #[error("provisioning profile: {0}")]
    Profile(String),
    #[error("merge: {0}")]
    Merge(String),
}

#[cfg(test)]
mod tests_cms;
#[cfg(test)]
mod tests_overlay;
#[cfg(test)]
mod tests_staged;
