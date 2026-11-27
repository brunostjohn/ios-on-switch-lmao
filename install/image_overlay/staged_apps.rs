// SPDX-License-Identifier: GPL-3.0-or-later
use std::cmp::Ordering;
use std::collections::HashMap;
use std::sync::Arc;

use plist::{Dictionary, Value};
use switch_apfs_write::{Attrs, InodeRef, Kind, Tree};

use crate::Error;
use crate::cms::verify_embedded_profile;
use crate::seed_manifest::{APP, IDENTIFIER, parse_seed_manifest, verify_source_app, verify_tree_app};
use crate::source::{SourceKind, TreeSource, is_apple_double, validate_ancestors};
use crate::tree_inspect::{
    CopyEntry, collect_tree_copy, index_children, is_tree_regular_file, read_installed, read_optional_source_plist,
    read_optional_tree_plist, read_source_info_plist, read_states, read_tree_info_plist, read_tree_regular_bytes,
    source_seed_apps,
};

pub const SEEDS: &str = "Switch/overlay/usr/share/Switch/Applications";
pub const STAGED: &str = "staged_system_apps";
pub const INSTALLED: &str = "containers/Bundle/Application";
pub const STATES: &str = "installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist";
pub const PLACEHOLDERS: &str = "Switch/overlay/System/Library/AppPlaceholders";
pub const SIGNATURES: &str = "Switch/overlay/System/Library/AppSignatures";
pub const MOBILE_UID: u32 = 501;
pub const MOBILE_GID: u32 = 501;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StagedApp {
    pub app_name: String,
    pub bundle_identifier: String,
    pub bundle_version: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StagedAppsReport {
    pub staged: Vec<StagedApp>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct AppVersion(Vec<String>);

impl AppVersion {
    fn parse(raw: &str) -> Result<Self, Error> {
        if raw.is_empty() {
            return Err(Error::StagedApp(format!("invalid staged app version: {raw:?}")));
        }
        let mut parts = Vec::new();
        for part in raw.split('.') {
            if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
                return Err(Error::StagedApp(format!("invalid staged app version: {raw:?}")));
            }
            parts.push(part.trim_start_matches('0').to_owned());
        }
        while parts.last().is_some_and(String::is_empty) {
            parts.pop();
        }
        Ok(Self(parts))
    }
}

impl Ord for AppVersion {
    fn cmp(&self, other: &Self) -> Ordering {
        for (a, b) in self.0.iter().zip(other.0.iter()) {
            let ord = a.len().cmp(&b.len()).then_with(|| a.cmp(b));
            if ord != Ordering::Equal {
                return ord;
            }
        }
        self.0.len().cmp(&other.0.len())
    }
}

impl PartialOrd for AppVersion {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

struct PlannedApp {
    info: StagedApp,
    root_mode: u16,
    entries: Vec<(String, CopyEntry)>,
}

/// Stages apps from `Switch/overlay/usr/share/Switch/Applications/*.app` already in `tree` into `staged_system_apps/`.
pub fn stage_apps(tree: &mut Tree, seed_pin: Option<&[u8]>) -> Result<StagedAppsReport, Error> {
    let children = index_children(tree);
    let installed = read_installed(tree, &children)?;
    let states = read_states(tree)?;

    let seeds_dir = match tree.lookup(SEEDS) {
        Some(dir) => {
            if tree.inode(dir).kind != Kind::Dir {
                return Err(Error::StagedApp(format!("{SEEDS} is not a directory")));
            }
            Some(dir)
        }
        None => None,
    };

    let installer_inode = seeds_dir.and_then(|dir| tree.child(dir, APP));
    let pin = match (seed_pin, installer_inode) {
        (None, None) => None,
        (None, Some(_)) => {
            return Err(Error::SeedPin(
                "cannot verify Installer seed: missing migration/installer-seed.json".into(),
            ));
        }
        (Some(bytes), None) => {
            let _ = parse_seed_manifest(bytes)?;
            return Err(Error::SeedPin("missing Installer seed or invalid manifest".into()));
        }
        (Some(bytes), Some(inode)) => {
            if tree.inode(inode).kind != Kind::Dir {
                return Err(Error::SeedPin("missing Installer seed or invalid manifest".into()));
            }
            Some(parse_seed_manifest(bytes)?)
        }
    };

    let Some(seeds_dir) = seeds_dir else {
        return Ok(StagedAppsReport::default());
    };

    let mut apps: Vec<(String, InodeRef)> = children
        .get(&seeds_dir)
        .into_iter()
        .flatten()
        .filter(|(name, _)| name.ends_with(".app"))
        .cloned()
        .collect();
    apps.sort_by(|a, b| a.0.cmp(&b.0));

    let mut planned = Vec::new();
    for (app_name, app_inode) in apps {
        if tree.inode(app_inode).kind != Kind::Dir {
            return Err(Error::StagedApp(format!("incomplete migration seed: {app_name}")));
        }
        let info = read_tree_info_plist(tree, app_inode, &app_name)?;
        let (identifier, version_str, version) =
            validate_app_metadata(&info, &app_name, |exec| is_tree_regular_file(tree, app_inode, exec))?;

        if app_name == APP || identifier == IDENTIFIER {
            let pin = pin.as_ref().ok_or_else(|| {
                Error::SeedPin("cannot verify Installer seed: missing migration/installer-seed.json".into())
            })?;
            verify_tree_app(tree, &children, app_inode, pin)?;
        }

        let placeholder_path = format!("{PLACEHOLDERS}/{app_name}/Info.plist");
        let placeholder = read_optional_tree_plist(tree, &placeholder_path)?;
        check_placeholder(&placeholder, &identifier, &version_str, &app_name)?;

        let sig_path = format!("{SIGNATURES}/{identifier}");
        if !tree
            .lookup(&sig_path)
            .is_some_and(|inode| tree.inode(inode).kind == Kind::File)
        {
            return Err(Error::StagedApp(format!(
                "migration seed has no detached signature: {app_name}"
            )));
        }

        if should_skip(&installed, &states, &identifier, &version)? {
            continue;
        }

        if let Some(profile_inode) = tree.child(app_inode, "embedded.mobileprovision") {
            let bytes = read_tree_regular_bytes(tree, profile_inode, &format!("{app_name}/embedded.mobileprovision"))?;
            verify_embedded_profile(&app_name, &bytes)?;
        }

        let mut entries = Vec::new();
        collect_tree_copy(tree, &children, app_inode, "", &mut entries)?;
        planned.push(PlannedApp {
            info: StagedApp {
                app_name,
                bundle_identifier: identifier,
                bundle_version: version_str,
            },
            root_mode: tree.inode(app_inode).attrs.mode,
            entries,
        });
    }

    place_planned(tree, planned)
}

/// Stages apps from an overlay `TreeSource` directly into `staged_system_apps/` on `tree`.
pub fn stage_apps_from_source(
    tree: &mut Tree,
    overlay: &TreeSource,
    seed_pin: Option<&[u8]>,
) -> Result<StagedAppsReport, Error> {
    validate_ancestors(overlay.entries())?;
    let children = index_children(tree);
    let installed = read_installed(tree, &children)?;
    let states = read_states(tree)?;

    let seeds_prefix = "usr/share/Switch/Applications";
    let app_names = source_seed_apps(overlay, seeds_prefix)?;
    let has_installer = app_names.iter().any(|name| name == APP);

    let pin = match (seed_pin, has_installer) {
        (None, false) => None,
        (None, true) => {
            return Err(Error::SeedPin(
                "cannot verify Installer seed: missing migration/installer-seed.json".into(),
            ));
        }
        (Some(bytes), false) => {
            let _ = parse_seed_manifest(bytes)?;
            return Err(Error::SeedPin("missing Installer seed or invalid manifest".into()));
        }
        (Some(bytes), true) => Some(parse_seed_manifest(bytes)?),
    };

    let mut planned = Vec::new();
    for app_name in app_names {
        let app_prefix = format!("{seeds_prefix}/{app_name}");
        if overlay.get(&app_prefix).is_some_and(|e| e.kind != SourceKind::Dir) {
            return Err(Error::StagedApp(format!("incomplete migration seed: {app_name}")));
        }
        let info_path = format!("{app_prefix}/Info.plist");
        let info = read_source_info_plist(overlay, &info_path, &app_name)?;
        let (identifier, version_str, version) = validate_app_metadata(&info, &app_name, |exec| {
            let path = format!("{app_prefix}/{exec}");
            matches!(overlay.get(&path).map(|e| &e.kind), Some(SourceKind::File(_)))
        })?;

        if app_name == APP || identifier == IDENTIFIER {
            let pin = pin.as_ref().ok_or_else(|| {
                Error::SeedPin("cannot verify Installer seed: missing migration/installer-seed.json".into())
            })?;
            verify_source_app(overlay, &app_prefix, pin, true)?;
        }

        let placeholder_path = format!("System/Library/AppPlaceholders/{app_name}/Info.plist");
        let placeholder = read_optional_source_plist(overlay, &placeholder_path)?;
        check_placeholder(&placeholder, &identifier, &version_str, &app_name)?;

        let sig_path = format!("System/Library/AppSignatures/{identifier}");
        if !matches!(overlay.get(&sig_path).map(|e| &e.kind), Some(SourceKind::File(_))) {
            return Err(Error::StagedApp(format!(
                "migration seed has no detached signature: {app_name}"
            )));
        }

        if should_skip(&installed, &states, &identifier, &version)? {
            continue;
        }

        let profile_path = format!("{app_prefix}/embedded.mobileprovision");
        if let Some(entry) = overlay.get(&profile_path) {
            let SourceKind::File(bytes) = &entry.kind else {
                return Err(Error::Profile(format!(
                    "{app_name}: embedded provisioning profile is not a decodable CMS profile"
                )));
            };
            verify_embedded_profile(&app_name, bytes)?;
        }

        let root_mode = overlay.get(&app_prefix).map_or(0o755, |e| e.mode & 0o777);
        let mut entries = Vec::new();
        let slash = format!("{app_prefix}/");
        for (path, entry) in overlay.entries() {
            if is_apple_double(path) {
                continue;
            }
            let Some(rel) = path.strip_prefix(&slash) else {
                continue;
            };
            let item = match &entry.kind {
                SourceKind::Dir => CopyEntry::Dir {
                    mode: entry.mode & 0o777,
                },
                SourceKind::File(bytes) => CopyEntry::File {
                    mode: entry.mode & 0o777,
                    bytes: Arc::clone(bytes),
                },
                SourceKind::Symlink(target) => CopyEntry::Symlink {
                    mode: entry.mode & 0o777,
                    target: target.clone(),
                },
            };
            entries.push((rel.to_owned(), item));
        }
        entries.sort_by(|(a, _), (b, _)| {
            a.matches('/')
                .count()
                .cmp(&b.matches('/').count())
                .then_with(|| a.cmp(b))
        });

        planned.push(PlannedApp {
            info: StagedApp {
                app_name,
                bundle_identifier: identifier,
                bundle_version: version_str,
            },
            root_mode,
            entries,
        });
    }

    place_planned(tree, planned)
}

fn place_planned(tree: &mut Tree, planned: Vec<PlannedApp>) -> Result<StagedAppsReport, Error> {
    if planned.is_empty() {
        return Ok(StagedAppsReport::default());
    }
    let root = tree.root();
    if let Some(existing_staged) = tree.child(root, STAGED) {
        if tree.inode(existing_staged).kind != Kind::Dir {
            return Err(Error::StagedApp(format!("{STAGED} is not a directory")));
        }
        for app in &planned {
            if tree.child(existing_staged, &app.info.app_name).is_some() {
                return Err(Error::StagedApp(format!(
                    "{STAGED}/{} already exists",
                    app.info.app_name
                )));
            }
        }
    }

    let staged_dir = match tree.child(root, STAGED) {
        Some(dir) => {
            tree.inode_mut(dir).attrs = Attrs::new(MOBILE_UID, MOBILE_GID, 0o755);
            dir
        }
        None => tree.add_dir(root, STAGED, Attrs::new(MOBILE_UID, MOBILE_GID, 0o755))?,
    };

    let mut staged = Vec::with_capacity(planned.len());
    for app in planned {
        let app_dir = tree.add_dir(
            staged_dir,
            &app.info.app_name,
            Attrs::new(MOBILE_UID, MOBILE_GID, app.root_mode),
        )?;
        let mut dirs: HashMap<String, InodeRef> = HashMap::new();
        dirs.insert(String::new(), app_dir);
        for (rel, entry) in app.entries {
            let (parent_path, name) = match rel.rsplit_once('/') {
                Some((p, n)) => (p, n),
                None => ("", rel.as_str()),
            };
            let parent = ensure_mobile_dir(tree, &mut dirs, parent_path)?;
            match entry {
                CopyEntry::Dir { mode } => {
                    if let Some(&existing) = dirs.get(&rel) {
                        tree.inode_mut(existing).attrs = Attrs::new(MOBILE_UID, MOBILE_GID, mode);
                    } else {
                        let inode = tree.add_dir(parent, name, Attrs::new(MOBILE_UID, MOBILE_GID, mode))?;
                        dirs.insert(rel, inode);
                    }
                }
                CopyEntry::File { mode, bytes } => {
                    let stream = if bytes.is_empty() {
                        None
                    } else {
                        Some(tree.add_bytes(bytes))
                    };
                    tree.add_file(parent, name, Attrs::new(MOBILE_UID, MOBILE_GID, mode), stream)?;
                }
                CopyEntry::Symlink { mode, target } => {
                    tree.add_symlink(parent, name, Attrs::new(MOBILE_UID, MOBILE_GID, mode), &target)?;
                }
            }
        }
        staged.push(app.info);
    }

    Ok(StagedAppsReport { staged })
}

fn ensure_mobile_dir(tree: &mut Tree, dirs: &mut HashMap<String, InodeRef>, rel: &str) -> Result<InodeRef, Error> {
    if let Some(&inode) = dirs.get(rel) {
        return Ok(inode);
    }
    let (parent_path, name) = match rel.rsplit_once('/') {
        Some((p, n)) => (p, n),
        None => ("", rel),
    };
    let parent = ensure_mobile_dir(tree, dirs, parent_path)?;
    let inode = tree.add_dir(parent, name, Attrs::new(MOBILE_UID, MOBILE_GID, 0o755))?;
    dirs.insert(rel.to_owned(), inode);
    Ok(inode)
}

fn validate_app_metadata(
    info: &Dictionary,
    app_name: &str,
    has_executable: impl Fn(&str) -> bool,
) -> Result<(String, String, AppVersion), Error> {
    let identifier = info
        .get("CFBundleIdentifier")
        .and_then(Value::as_string)
        .filter(|s| !s.is_empty());
    let executable = info
        .get("CFBundleExecutable")
        .and_then(Value::as_string)
        .filter(|s| !s.is_empty());
    let (Some(identifier), Some(executable)) = (identifier, executable) else {
        return Err(Error::StagedApp(format!("incomplete migration seed: {app_name}")));
    };
    if !has_executable(executable) {
        return Err(Error::StagedApp(format!("incomplete migration seed: {app_name}")));
    }
    let version_str = info
        .get("CFBundleVersion")
        .and_then(Value::as_string)
        .ok_or_else(|| Error::StagedApp(format!("invalid staged app version in {app_name}")))?;
    let version = AppVersion::parse(version_str)?;
    Ok((identifier.to_owned(), version_str.to_owned(), version))
}

fn check_placeholder(
    placeholder: &Dictionary,
    identifier: &str,
    version_str: &str,
    app_name: &str,
) -> Result<(), Error> {
    let pid = placeholder.get("CFBundleIdentifier").and_then(Value::as_string);
    let pver = placeholder.get("CFBundleVersion").and_then(Value::as_string);
    if pid != Some(identifier) || pver != Some(version_str) {
        return Err(Error::StagedApp(format!(
            "migration seed has no matching system app placeholder: {app_name}"
        )));
    }
    Ok(())
}

fn should_skip(
    installed: &HashMap<String, String>,
    states: &HashMap<String, i64>,
    identifier: &str,
    version: &AppVersion,
) -> Result<bool, Error> {
    if let Some(current_ver) = installed.get(identifier) {
        if AppVersion::parse(current_ver)? >= *version {
            return Ok(true);
        }
    } else if states.get(identifier) == Some(&2) {
        return Ok(true);
    }
    Ok(false)
}
