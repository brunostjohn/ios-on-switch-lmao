// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::BufReader;
use std::path::Path;

use switch_apfs_read::{Entry, Fs, Stream, XattrData, stream};
use switch_boot::{Assets, LAUNCHD_CATALOG, LaunchdCatalog, Members};
use switch_catalog::Build;

use crate::LocalInputs;
use crate::error::{EngineError, Result};
use crate::fetch::Im4pMembers;
use crate::progress::EngineProgress;

pub struct BootOutputs {
    pub sd_files: BTreeMap<String, Vec<u8>>,
    pub overlay_files: BTreeMap<String, Vec<u8>>,
    pub installd_cdhash: Option<[u8; 20]>,
}

pub fn prepare_boot(
    build: &Build,
    local: &LocalInputs,
    boot_tar: Option<&Path>,
    im4p: Option<&Im4pMembers>,
    system_fs: &Fs<'_>,
    progress: &mut EngineProgress<'_>,
) -> Result<BootOutputs> {
    progress.check_cancel()?;
    if let Some(dir) = &local.boot_files_dir {
        progress.log(format!("Loading prebuilt boot files from {}", dir.display()));
        return load_boot_dir(dir);
    }

    let tar_path = boot_tar
        .ok_or_else(|| EngineError::Config("boot asset tar is required when --boot-files-dir is not set".into()))?;
    let im4p = im4p
        .ok_or_else(|| EngineError::Config("IPSW IM4P members are required when --boot-files-dir is not set".into()))?;

    let boot_asset = build.asset("boot")?;
    let tar_file = File::open(tar_path).map_err(|e| EngineError::io_path(tar_path, e))?;
    // Hash once against the catalog asset digest while unpacking; members inside the tar are not hashed a second time.
    let assets = Assets::from_tar_checked(BufReader::new(tar_file), boot_asset)?;

    let installd = read_system_file(system_fs, switch_installd::PATH)?;
    let system_version = read_system_file(system_fs, switch_installd::SYSTEM_VERSION)?;
    let launchd_pair = if build.steps.launchd_catalog {
        let plist = read_system_file(system_fs, LAUNCHD_CATALOG)?;
        let sig = read_system_file(system_fs, &format!("{LAUNCHD_CATALOG}.sig"))?;
        Some((plist, sig))
    } else {
        None
    };

    let launchd_catalog = launchd_pair.as_ref().map(|(plist, signature)| LaunchdCatalog {
        plist: plist.as_slice(),
        signature: signature.as_slice(),
    });
    let members = Members {
        kernelcache: &im4p.kernelcache,
        device_tree: &im4p.device_tree,
        static_trust_cache: &im4p.static_trust_cache,
        system_cryptex_trust_cache: &im4p.system_cryptex_trust_cache,
        app_cryptex_trust_cache: &im4p.app_cryptex_trust_cache,
        installd: &installd,
        system_version: &system_version,
        launchd_catalog,
    };

    progress.log(format!("Building boot package for {}", build.build()));
    let package = switch_boot::boot_files(build, &assets, &members)?;
    let mut sd_files = BTreeMap::new();
    let mut overlay_files = BTreeMap::new();
    for (path, data) in package.files {
        if let Some(rel) = path.strip_prefix("overlay/") {
            overlay_files.insert(rel.to_owned(), data);
        } else {
            sd_files.insert(path, data);
        }
    }
    Ok(BootOutputs {
        sd_files,
        overlay_files,
        installd_cdhash: Some(package.installd_cdhash),
    })
}

pub fn read_system_file(system_fs: &Fs<'_>, path: &str) -> Result<Vec<u8>> {
    let oid = system_fs
        .lookup(path)?
        .ok_or_else(|| EngineError::Config(format!("{path} is missing from System volume")))?;
    let (entry, _) = system_fs.entry(oid, path.to_owned(), None)?;
    if !entry.inode.is_regular() {
        return Err(EngineError::Config(format!("{path} is not a regular file")));
    }
    read_entry_plaintext(system_fs, &entry)
}

fn read_entry_plaintext(system_fs: &Fs<'_>, entry: &Entry) -> Result<Vec<u8>> {
    if entry.inode.is_compressed() {
        let decmpfs = read_xattr(system_fs, entry, switch_decmpfs::XATTR)?.ok_or_else(|| {
            EngineError::ApfsRead(switch_apfs_read::ReadError::invalid(format!(
                "{}: missing decmpfs xattr",
                entry.path
            )))
        })?;
        let fork = read_xattr(system_fs, entry, switch_decmpfs::RESOURCE_FORK)?;
        return Ok(switch_decmpfs::decompress(&decmpfs, fork.as_deref())?);
    }
    let Some(s) = &entry.data else {
        return Ok(Vec::new());
    };
    read_stream_vec(system_fs, s)
}

fn read_xattr(system_fs: &Fs<'_>, entry: &Entry, name: &str) -> Result<Option<Vec<u8>>> {
    let Some((xattr, xstream)) = entry.xattr(name) else {
        return Ok(None);
    };
    match (&xattr.data, xstream) {
        (XattrData::Embedded(bytes), _) => Ok(Some(bytes.clone())),
        (XattrData::Stream { .. }, Some(s)) => Ok(Some(read_stream_vec(system_fs, s)?)),
        (XattrData::Stream { id, .. }, None) => Err(EngineError::ApfsRead(switch_apfs_read::ReadError::invalid(
            format!("{}: {name} stream {id} missing extents", entry.path),
        ))),
    }
}

fn read_stream_vec(system_fs: &Fs<'_>, s: &Stream) -> Result<Vec<u8>> {
    let size = usize::try_from(s.size)
        .map_err(|_| EngineError::Config(format!("APFS stream {} ({} bytes) too large", s.id, s.size)))?;
    let mut out = Vec::with_capacity(size);
    stream::read(system_fs.source(), s, &mut |chunk| out.extend_from_slice(chunk))?;
    Ok(out)
}

fn load_boot_dir(root: &Path) -> Result<BootOutputs> {
    let mut sd_files = BTreeMap::new();
    let mut overlay_files = BTreeMap::new();
    walk_boot_dir(root, "", &mut sd_files, &mut overlay_files)?;
    if sd_files.is_empty() {
        return Err(EngineError::Config(format!(
            "boot files directory {} contains no FAT boot files",
            root.display()
        )));
    }
    Ok(BootOutputs {
        sd_files,
        overlay_files,
        installd_cdhash: None,
    })
}

fn walk_boot_dir(
    dir: &Path,
    prefix: &str,
    sd_files: &mut BTreeMap<String, Vec<u8>>,
    overlay_files: &mut BTreeMap<String, Vec<u8>>,
) -> Result<()> {
    let mut entries: Vec<_> = fs::read_dir(dir)
        .map_err(|e| EngineError::io_path(dir, e))?
        .collect::<std::result::Result<Vec<_>, _>>()
        .map_err(|e| EngineError::io_path(dir, e))?;
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| EngineError::Config(format!("{}: non-UTF-8 name", entry.path().display())))?;
        let rel = if prefix.is_empty() {
            name
        } else {
            format!("{prefix}/{name}")
        };
        let path = entry.path();
        let meta = fs::metadata(&path).map_err(|e| EngineError::io_path(&path, e))?;
        if meta.is_dir() {
            walk_boot_dir(&path, &rel, sd_files, overlay_files)?;
        } else if meta.is_file() {
            let bytes = fs::read(&path).map_err(|e| EngineError::io_path(&path, e))?;
            if let Some(ov) = rel.strip_prefix("overlay/") {
                overlay_files.insert(ov.to_owned(), bytes);
            } else {
                sd_files.insert(rel, bytes);
            }
        }
    }
    Ok(())
}
