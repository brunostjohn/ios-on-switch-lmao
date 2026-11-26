// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use flate2::{Compression, GzBuilder};
use sha2::{Digest, Sha256};
use switch_apfs_read::Fs;
use switch_apfs_write::{Attrs, Content, InodeRef, Kind, StreamRef, Tree};
use switch_image::{FileFill, hash_file};

use crate::Error;
use crate::manifest::{
    ADDONS_DIR, FileContent, FileRecord, GpuStage, ICON_CACHE_RECORD_PATH, REQUIRED_PLUGIN_PATH, STATE_PATH, check_path,
};
use crate::plist_merge::prepare_merged_plist;

pub const LARGE_FILE_THRESHOLD: u64 = 1 << 20;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GpuStageReport {
    pub files: usize,
    pub prefs: usize,
    pub whole_digest: String,
    pub icon_build: String,
}

enum PreparedContent {
    Empty,
    Bytes(Arc<[u8]>),
    DiskFill { path: PathBuf, size: u64, sha256: [u8; 32] },
}

struct PreparedFile<'a> {
    path: &'a str,
    mode: u16,
    content: PreparedContent,
}

struct PreparedPref<'a> {
    path: &'a str,
    owner_uid: u32,
    bytes: Vec<u8>,
}

pub fn stage_gpu_from_path(tree: &mut Tree, source_fs: Option<&Fs<'_>>, path: &Path) -> Result<GpuStageReport, Error> {
    let stage = GpuStage::from_path(path)?;
    stage_gpu(tree, source_fs, &stage)
}

pub fn stage_gpu(tree: &mut Tree, source_fs: Option<&Fs<'_>>, stage: &GpuStage) -> Result<GpuStageReport, Error> {
    if !stage.files.contains_key(REQUIRED_PLUGIN_PATH) {
        return Err(Error::MissingPlugin);
    }

    let mut prepared_files = Vec::with_capacity(stage.files.len());
    for record in stage.files.values() {
        check_path(&record.path)?;
        if record.mode > 0o7777 {
            return Err(Error::Manifest(format!(
                "{}: mode {:#o} exceeds 0o7777",
                record.path, record.mode
            )));
        }
        check_target_file_slot(tree, &record.path)?;
        let content = verify_and_prepare_file(record)?;
        prepared_files.push(PreparedFile {
            path: &record.path,
            mode: record.mode,
            content,
        });
    }

    check_dir_slot(tree, "ngpu")?;
    check_dir_slot(tree, ADDONS_DIR)?;
    check_target_file_slot(tree, STATE_PATH)?;
    check_dir_slot(tree, "Switch")?;
    check_target_file_slot(tree, ICON_CACHE_RECORD_PATH)?;

    let mut prepared_prefs = Vec::with_capacity(stage.prefs.len());
    for (plist_path, pref) in &stage.prefs {
        check_path(plist_path)?;
        check_target_file_slot(tree, plist_path)?;
        let bytes = prepare_merged_plist(tree, source_fs, plist_path, pref)?;
        prepared_prefs.push(PreparedPref {
            path: plist_path,
            owner_uid: pref.owner_uid,
            bytes,
        });
    }

    let whole_digest = stage.whole_digest();
    let icon_build = stage.icon_build();
    let stage_gz = gzip_deterministic(stage.stage_tsv().as_bytes())?;
    let icon_record = format!("{icon_build}\n").into_bytes();

    ensure_owned_dir(tree, "ngpu", 0, 0, 0o755)?;
    ensure_owned_dir(tree, ADDONS_DIR, 0, 0, 0o755)?;

    for item in prepared_files {
        let stream = match item.content {
            PreparedContent::Empty => None,
            PreparedContent::Bytes(bytes) => Some(tree.add_bytes(bytes)),
            PreparedContent::DiskFill { path, size, sha256 } => {
                let fill = Arc::new(FileFill::new(path, size, Some(sha256)));
                Some(tree.add_stream(size, Content::Fill(fill)))
            }
        };
        write_file_preserving_existing_dirs(tree, item.path, Attrs::new(0, 0, item.mode), stream)?;
    }

    let gz_stream = Some(tree.add_bytes(stage_gz));
    write_file_preserving_existing_dirs(tree, STATE_PATH, Attrs::new(0, 0, 0o644), gz_stream)?;

    let icon_stream = Some(tree.add_bytes(icon_record));
    write_file_preserving_existing_dirs(tree, ICON_CACHE_RECORD_PATH, Attrs::new(0, 0, 0o644), icon_stream)?;

    for pref in prepared_prefs {
        let stream = Some(tree.add_bytes(pref.bytes));
        write_file_preserving_existing_dirs(
            tree,
            pref.path,
            Attrs::new(pref.owner_uid, pref.owner_uid, 0o644),
            stream,
        )?;
    }

    Ok(GpuStageReport {
        files: stage.files.len(),
        prefs: stage.prefs.len(),
        whole_digest,
        icon_build,
    })
}

fn verify_and_prepare_file(record: &FileRecord) -> Result<PreparedContent, Error> {
    match &record.content {
        FileContent::Bytes(bytes) => {
            let got_size = bytes.len() as u64;
            if got_size != record.size {
                return Err(Error::SizeMismatch {
                    path: record.path.clone(),
                    got: got_size,
                    want: record.size,
                });
            }
            let got_sha: [u8; 32] = Sha256::digest(bytes).into();
            if got_sha != record.sha256 {
                return Err(Error::DigestMismatch {
                    path: record.path.clone(),
                    got: hex::encode(got_sha),
                    want: hex::encode(record.sha256),
                });
            }
            if bytes.is_empty() {
                Ok(PreparedContent::Empty)
            } else {
                Ok(PreparedContent::Bytes(Arc::clone(bytes)))
            }
        }
        FileContent::Path(disk_path) => {
            let meta = fs::metadata(disk_path).map_err(|e| {
                if e.kind() == io::ErrorKind::NotFound {
                    Error::MissingSourceFile(disk_path.display().to_string())
                } else {
                    Error::Io(e)
                }
            })?;
            if !meta.is_file() {
                return Err(Error::NotRegularFile(disk_path.display().to_string()));
            }
            if meta.len() != record.size {
                return Err(Error::SizeMismatch {
                    path: record.path.clone(),
                    got: meta.len(),
                    want: record.size,
                });
            }
            if record.size == 0 {
                let got_sha: [u8; 32] = Sha256::digest([]).into();
                if got_sha != record.sha256 {
                    return Err(Error::DigestMismatch {
                        path: record.path.clone(),
                        got: hex::encode(got_sha),
                        want: hex::encode(record.sha256),
                    });
                }
                return Ok(PreparedContent::Empty);
            }
            if record.size >= LARGE_FILE_THRESHOLD {
                let (got_size, got_sha) = hash_file(disk_path)?;
                if got_size != record.size {
                    return Err(Error::SizeMismatch {
                        path: record.path.clone(),
                        got: got_size,
                        want: record.size,
                    });
                }
                if got_sha != record.sha256 {
                    return Err(Error::DigestMismatch {
                        path: record.path.clone(),
                        got: hex::encode(got_sha),
                        want: hex::encode(record.sha256),
                    });
                }
                Ok(PreparedContent::DiskFill {
                    path: disk_path.clone(),
                    size: record.size,
                    sha256: record.sha256,
                })
            } else {
                let bytes = fs::read(disk_path)?;
                let got_size = bytes.len() as u64;
                if got_size != record.size {
                    return Err(Error::SizeMismatch {
                        path: record.path.clone(),
                        got: got_size,
                        want: record.size,
                    });
                }
                let got_sha: [u8; 32] = Sha256::digest(&bytes).into();
                if got_sha != record.sha256 {
                    return Err(Error::DigestMismatch {
                        path: record.path.clone(),
                        got: hex::encode(got_sha),
                        want: hex::encode(record.sha256),
                    });
                }
                Ok(PreparedContent::Bytes(Arc::from(bytes)))
            }
        }
    }
}

fn check_dir_slot(tree: &Tree, path: &str) -> Result<(), Error> {
    let mut cur = tree.root();
    let mut built = String::new();
    for part in path.split('/') {
        if !built.is_empty() {
            built.push('/');
        }
        built.push_str(part);
        let Some(next) = tree.child(cur, part) else {
            return Ok(());
        };
        if tree.inode(next).kind != Kind::Dir {
            return Err(Error::NotDirectory(built));
        }
        cur = next;
    }
    Ok(())
}

fn check_target_file_slot(tree: &Tree, path: &str) -> Result<(), Error> {
    let mut cur = tree.root();
    let mut built = String::new();
    let mut parts = path.split('/').peekable();
    while let Some(part) = parts.next() {
        if !built.is_empty() {
            built.push('/');
        }
        built.push_str(part);
        let Some(next) = tree.child(cur, part) else {
            return Ok(());
        };
        if parts.peek().is_some() {
            if tree.inode(next).kind != Kind::Dir {
                return Err(Error::NotDirectory(built));
            }
            cur = next;
        } else if tree.inode(next).kind != Kind::File {
            return Err(Error::NotRegularFile(built));
        }
    }
    Ok(())
}

fn ensure_owned_dir(tree: &mut Tree, path: &str, uid: u32, gid: u32, mode: u16) -> Result<InodeRef, Error> {
    let mut cur = tree.root();
    let mut built = String::new();
    for part in path.split('/') {
        if !built.is_empty() {
            built.push('/');
        }
        built.push_str(part);
        cur = match tree.child(cur, part) {
            Some(existing) => {
                if tree.inode(existing).kind != Kind::Dir {
                    return Err(Error::NotDirectory(built));
                }
                let attrs = &mut tree.inode_mut(existing).attrs;
                attrs.uid = uid;
                attrs.gid = gid;
                attrs.mode = mode;
                existing
            }
            None => tree.add_dir(cur, part, Attrs::new(uid, gid, mode))?,
        };
    }
    Ok(cur)
}

fn write_file_preserving_existing_dirs(
    tree: &mut Tree,
    path: &str,
    file_attrs: Attrs,
    data: Option<StreamRef>,
) -> Result<InodeRef, Error> {
    let (parent_path, name) = match path.rsplit_once('/') {
        Some((p, n)) => (Some(p), n),
        None => (None, path),
    };
    let mut parent = tree.root();
    if let Some(rel) = parent_path {
        let mut built = String::new();
        for part in rel.split('/') {
            if !built.is_empty() {
                built.push('/');
            }
            built.push_str(part);
            parent = match tree.child(parent, part) {
                Some(existing) => {
                    if tree.inode(existing).kind != Kind::Dir {
                        return Err(Error::NotDirectory(built));
                    }
                    existing
                }
                None => tree.add_dir(parent, part, Attrs::new(0, 0, 0o755))?,
            };
        }
    }

    if let Some(existing) = tree.child(parent, name) {
        if tree.inode(existing).kind != Kind::File {
            return Err(Error::NotRegularFile(path.to_owned()));
        }
        tree.replace_content(existing, data)?;
        let attrs = &mut tree.inode_mut(existing).attrs;
        attrs.uid = file_attrs.uid;
        attrs.gid = file_attrs.gid;
        attrs.mode = file_attrs.mode;
        attrs.times = None;
        Ok(existing)
    } else {
        Ok(tree.add_file(parent, name, file_attrs, data)?)
    }
}

fn gzip_deterministic(data: &[u8]) -> Result<Vec<u8>, Error> {
    let mut encoder = GzBuilder::new().mtime(0).write(Vec::new(), Compression::fast());
    encoder.write_all(data)?;
    Ok(encoder.finish()?)
}
