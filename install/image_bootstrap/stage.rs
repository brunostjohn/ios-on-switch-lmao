// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File};
use std::io::{Cursor, Read};
use std::path::Path;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_format::fs_records::{SYMLINK_XATTR_NAME, XATTR_DATA_EMBEDDED, XATTR_FILE_SYSTEM_OWNED};
use switch_apfs_write::{Attrs, Content, InodeRef, Kind, Tree, Xattr, XattrValue};

use crate::Error;
use crate::archive::{MetadataEntry, TarEntryKind, TarMember, build_metadata, read_and_validate_archive};
use crate::dpkg::rewrite_status;
use crate::json_py::format_python_json;
use crate::manifest::{
    ARCHIVE, BootstrapPins, DIRECTORY, HELPER, MANIFEST, METADATA, MIGRATION_PENDING, ROOT, is_build_id, parse_manifest,
};

const REQUIRED_MARKERS: [&str; 2] = [".procursus_strapped", ".switch-bootstrap-ready"];
const DPKG_STATUS: &str = "Library/dpkg/status";
const OVERLAY_HELPER_PATH: &str = "Switch/overlay/usr/libexec/switch-bootstrap";

pub struct BootstrapInput<'a> {
    pub manifest_json: &'a [u8],
    pub archive: &'a [u8],
    /// Bytes of `overlay/usr/libexec/switch-bootstrap`; when `None`, looked up in `tree`.
    pub helper: Option<&'a [u8]>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BootstrapReport {
    pub build: String,
    pub firmware: String,
    pub entries: usize,
    pub migration_pending: Option<String>,
}

/// Stages bootstrap from a package directory (`<package>/bootstrap/` + `<package>/overlay/usr/libexec/switch-bootstrap`).
/// Returns `Ok(None)` when `<package>/bootstrap` does not exist.
pub fn stage_bootstrap_from_dir(
    tree: &mut Tree,
    package: &Path,
    pins: &BootstrapPins<'_>,
) -> Result<Option<BootstrapReport>, Error> {
    let dir = package.join("bootstrap");
    if !dir.exists() {
        return Ok(None);
    }
    let manifest_bytes =
        fs::read(dir.join(MANIFEST)).map_err(|e| Error::Manifest(format!("{}: {e}", dir.display())))?;
    let helper_path = package.join("overlay/usr/libexec").join(HELPER);
    let helper_bytes =
        fs::read(&helper_path).map_err(|e| Error::Manifest(format!("{}: {e}", helper_path.display())))?;
    let archive_file = File::open(dir.join(ARCHIVE)).map_err(|e| Error::Manifest(format!("{}: {e}", dir.display())))?;
    let report = stage_bootstrap_reader(tree, &manifest_bytes, archive_file, Some(&helper_bytes), pins)?;
    Ok(Some(report))
}

/// Stages bootstrap from in-memory slices onto `tree`.
pub fn stage_bootstrap(
    tree: &mut Tree,
    input: &BootstrapInput<'_>,
    pins: &BootstrapPins<'_>,
) -> Result<BootstrapReport, Error> {
    stage_bootstrap_reader(
        tree,
        input.manifest_json,
        Cursor::new(input.archive),
        input.helper,
        pins,
    )
}

/// Stages bootstrap streaming `payload.tar` from `archive` directly onto `tree`.
pub fn stage_bootstrap_reader(
    tree: &mut Tree,
    manifest_json: &[u8],
    archive: impl Read,
    helper: Option<&[u8]>,
    pins: &BootstrapPins<'_>,
) -> Result<BootstrapReport, Error> {
    let manifest = parse_manifest(manifest_json, Some(pins))?;

    let helper_hash = match helper {
        Some(bytes) => hex::encode(Sha256::digest(bytes)),
        None => hash_tree_helper(tree)?,
    };
    if helper_hash != manifest.helper_sha256() {
        return Err(Error::Digest(format!("{HELPER} SHA-256 differs from its manifest")));
    }

    let jb_target = format!("/private/var/{DIRECTORY}/{}/{ROOT}", pins.build);
    check_tree_preconditions(tree, pins.build, &jb_target)?;

    let mut members = read_and_validate_archive(archive, Some(manifest.archive_sha256()))?;

    for marker in REQUIRED_MARKERS {
        let valid = members
            .get(marker)
            .is_some_and(|m| matches!(m.kind, TarEntryKind::File(_) | TarEntryKind::HardLink(_)));
        if !valid {
            return Err(Error::Archive(format!("payload lacks regular {marker}")));
        }
    }

    rewrite_dpkg_in_members(&mut members, pins.version)?;

    let metadata = build_metadata(&members);
    let installed_manifest = format_python_json(&manifest.with_installed_pins(pins.build, pins.version));
    let metadata_json = format_python_json(&serde_json::to_value(&metadata).expect("metadata serializes"));

    let root = tree.root();
    let switch_dir = ensure_root_dir(tree, root, "Switch")?;
    let bootstrap_dir = ensure_root_dir(tree, switch_dir, "bootstrap")?;
    let build_dir = tree.add_dir(bootstrap_dir, pins.build, Attrs::new(0, 0, 0o755))?;
    let payload_root = tree.add_dir(build_dir, ROOT, Attrs::new(0, 0, 0o755))?;

    populate_root(tree, payload_root, &members, &metadata)?;

    let manifest_stream = tree.add_bytes(installed_manifest.into_bytes());
    tree.add_file(build_dir, MANIFEST, Attrs::new(0, 0, 0o644), Some(manifest_stream))?;

    let metadata_stream = tree.add_bytes(metadata_json.into_bytes());
    tree.add_file(build_dir, METADATA, Attrs::new(0, 0, 0o644), Some(metadata_stream))?;

    let migration_pending = if let Some(generation) = manifest.managed_generation() {
        let bytes = format!("{generation}\n").into_bytes();
        let stream = tree.add_bytes(bytes);
        tree.add_file(build_dir, MIGRATION_PENDING, Attrs::new(0, 0, 0o644), Some(stream))?;
        Some(generation.to_owned())
    } else {
        None
    };

    activate_jb_symlink(tree, &jb_target)?;

    Ok(BootstrapReport {
        build: pins.build.to_owned(),
        firmware: pins.version.to_owned(),
        entries: metadata.len(),
        migration_pending,
    })
}

fn hash_tree_helper(tree: &Tree) -> Result<String, Error> {
    let inode = tree
        .lookup(OVERLAY_HELPER_PATH)
        .ok_or_else(|| Error::Manifest(format!("missing {OVERLAY_HELPER_PATH}")))?;
    let node = tree.inode(inode);
    if node.kind != Kind::File {
        return Err(Error::Manifest(format!("{OVERLAY_HELPER_PATH} is not a regular file")));
    }
    let bytes: &[u8] = match node.data {
        None => &[],
        Some(stream) => match &tree.stream(stream).content {
            Content::Bytes(b) => b.as_ref(),
            _ => {
                return Err(Error::Manifest(format!(
                    "{OVERLAY_HELPER_PATH}: content is not in memory"
                )));
            }
        },
    };
    Ok(hex::encode(Sha256::digest(bytes)))
}

fn is_managed_jb_target(target: &str) -> bool {
    let prefix = format!("/private/var/{DIRECTORY}/");
    let Some(rest) = target.strip_prefix(&prefix) else {
        return false;
    };
    let Some(build) = rest.strip_suffix(&format!("/{ROOT}")) else {
        return false;
    };
    is_build_id(build)
}

fn read_symlink_target(tree: &Tree, inode: InodeRef) -> Result<String, Error> {
    let xattr = tree
        .inode(inode)
        .xattrs
        .iter()
        .find(|x| x.name == SYMLINK_XATTR_NAME)
        .ok_or_else(|| Error::Tree("/var/jb symlink missing target xattr".into()))?;
    let XattrValue::Embedded(bytes) = &xattr.value else {
        return Err(Error::Tree("/var/jb symlink target is not embedded".into()));
    };
    let raw = bytes.strip_suffix(&[0]).unwrap_or(bytes);
    let target = std::str::from_utf8(raw).map_err(|_| Error::Tree("/var/jb symlink target is not UTF-8".into()))?;
    Ok(target.to_owned())
}

fn check_tree_preconditions(tree: &Tree, build: &str, jb_target: &str) -> Result<(), Error> {
    let root = tree.root();
    if let Some(jb) = tree.child(root, "jb") {
        if tree.inode(jb).kind != Kind::Symlink {
            return Err(Error::Tree(
                "/var/jb already exists and is not a managed symlink".into(),
            ));
        }
        let current = read_symlink_target(tree, jb)?;
        if current != jb_target && !is_managed_jb_target(&current) {
            return Err(Error::Tree("/var/jb points outside the managed bootstrap".into()));
        }
    }
    if let Some(switch_dir) = tree.child(root, "Switch") {
        if tree.inode(switch_dir).kind != Kind::Dir {
            return Err(Error::Tree("Switch is not a real directory".into()));
        }
        if let Some(boot_dir) = tree.child(switch_dir, "bootstrap") {
            if tree.inode(boot_dir).kind != Kind::Dir {
                return Err(Error::Tree(format!("{DIRECTORY} is not a real directory")));
            }
            if tree.child(boot_dir, build).is_some() {
                return Err(Error::Tree(format!("{DIRECTORY}/{build} already exists")));
            }
        }
    }
    Ok(())
}

fn rewrite_dpkg_in_members(members: &mut BTreeMap<String, TarMember>, version: &str) -> Result<(), Error> {
    let target_key = match members.get(DPKG_STATUS).map(|m| &m.kind) {
        Some(TarEntryKind::File(_)) => DPKG_STATUS.to_owned(),
        Some(TarEntryKind::HardLink(target)) => target.clone(),
        _ => return Err(Error::Dpkg("missing regular dpkg status database".into())),
    };
    let bytes = match &members
        .get(&target_key)
        .ok_or_else(|| Error::Dpkg("missing regular dpkg status database".into()))?
        .kind
    {
        TarEntryKind::File(b) => Arc::clone(b),
        _ => return Err(Error::Dpkg("missing regular dpkg status database".into())),
    };
    let text = std::str::from_utf8(&bytes).map_err(|_| Error::Dpkg("dpkg status is not UTF-8".into()))?;
    let rewritten = rewrite_status(text, version)?;
    if let Some(member) = members.get_mut(&target_key) {
        member.kind = TarEntryKind::File(Arc::from(rewritten.into_bytes()));
    }
    Ok(())
}

fn ensure_root_dir(tree: &mut Tree, parent: InodeRef, name: &str) -> Result<InodeRef, Error> {
    if let Some(existing) = tree.child(parent, name) {
        let attrs = &mut tree.inode_mut(existing).attrs;
        attrs.uid = 0;
        attrs.gid = 0;
        attrs.mode = 0o755;
        return Ok(existing);
    }
    Ok(tree.add_dir(parent, name, Attrs::new(0, 0, 0o755))?)
}

fn populate_root(
    tree: &mut Tree,
    payload_root: InodeRef,
    members: &BTreeMap<String, TarMember>,
    metadata: &[MetadataEntry],
) -> Result<(), Error> {
    let mut dirs: HashMap<String, InodeRef> = HashMap::new();
    dirs.insert(String::new(), payload_root);

    let mut dir_items: Vec<&MetadataEntry> = metadata
        .iter()
        .filter(|m| m.kind == "directory" && !m.path.is_empty())
        .collect();
    dir_items.sort_by(|a, b| {
        a.path
            .matches('/')
            .count()
            .cmp(&b.path.matches('/').count())
            .then_with(|| a.path.cmp(&b.path))
    });

    for item in dir_items {
        let (parent_path, name) = split_parent(&item.path);
        let parent = dirs[parent_path];
        let inode = tree.add_dir(parent, name, Attrs::new(item.uid, item.gid, item.mode))?;
        dirs.insert(item.path.clone(), inode);
    }

    let mut files: HashMap<String, InodeRef> = HashMap::new();
    for (path, member) in members {
        let (parent_path, name) = split_parent(path);
        let parent = dirs[parent_path];
        match &member.kind {
            TarEntryKind::File(bytes) => {
                let stream = if bytes.is_empty() {
                    None
                } else {
                    Some(tree.add_bytes(Arc::clone(bytes)))
                };
                let inode = tree.add_file(parent, name, Attrs::new(member.uid, member.gid, member.mode), stream)?;
                files.insert(path.clone(), inode);
            }
            TarEntryKind::Symlink(target) => {
                tree.add_symlink(parent, name, Attrs::new(member.uid, member.gid, member.mode), target)?;
            }
            TarEntryKind::Dir | TarEntryKind::HardLink(_) => {}
        }
    }

    for (path, member) in members {
        if let TarEntryKind::HardLink(target_path) = &member.kind {
            let (parent_path, name) = split_parent(path);
            let parent = dirs[parent_path];
            let target_inode = files[target_path];
            tree.add_link(target_inode, parent, name)?;
        }
    }

    Ok(())
}

fn activate_jb_symlink(tree: &mut Tree, target: &str) -> Result<(), Error> {
    let root = tree.root();
    if let Some(existing) = tree.child(root, "jb") {
        let mut value = target.as_bytes().to_vec();
        value.push(0);
        tree.put_xattr(
            existing,
            Xattr {
                name: SYMLINK_XATTR_NAME.into(),
                flags: XATTR_DATA_EMBEDDED | XATTR_FILE_SYSTEM_OWNED,
                value: XattrValue::Embedded(value),
            },
        );
        let attrs = &mut tree.inode_mut(existing).attrs;
        attrs.uid = 0;
        attrs.gid = 0;
        return Ok(());
    }
    tree.add_symlink(root, "jb", Attrs::new(0, 0, 0o755), target)?;
    Ok(())
}

fn split_parent(path: &str) -> (&str, &str) {
    match path.rsplit_once('/') {
        Some((p, n)) => (p, n),
        None => ("", path),
    }
}
