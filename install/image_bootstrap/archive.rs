// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io::{self, Read};
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::Error;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TarEntryKind {
    Dir,
    File(Arc<[u8]>),
    Symlink(String),
    HardLink(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TarMember {
    pub uid: u32,
    pub gid: u32,
    pub mode: u16,
    pub kind: TarEntryKind,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MetadataEntry {
    pub gid: u32,
    pub kind: String,
    pub mode: u16,
    pub path: String,
    pub uid: u32,
}

struct HashingReader<R> {
    inner: R,
    hasher: Sha256,
}

impl<R: Read> HashingReader<R> {
    fn new(inner: R) -> Self {
        Self {
            inner,
            hasher: Sha256::new(),
        }
    }

    fn finish(mut self) -> io::Result<String> {
        io::copy(&mut self, &mut io::sink())?;
        Ok(hex::encode(self.hasher.finalize()))
    }
}

impl<R: Read> Read for HashingReader<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        if n > 0 {
            self.hasher.update(&buf[..n]);
        }
        Ok(n)
    }
}

/// Normalizes a tar archive path per `tools/install/bootstrap.py:_name`.
pub fn normalize_archive_name(raw: &str) -> Result<String, Error> {
    if raw.contains('\0') {
        return Err(Error::Archive(format!("unsafe archive path {raw:?}")));
    }
    let mut name = raw;
    while let Some(rest) = name.strip_prefix("./") {
        name = rest;
    }
    name = name.trim_end_matches('/');
    if name.is_empty() || name == "." {
        return Ok(String::new());
    }
    if name.starts_with('/') || name.len() >= 1024 {
        return Err(Error::Archive(format!("unsafe archive path {raw:?}")));
    }
    for part in name.split('/') {
        if part.is_empty() || part == "." || part == ".." {
            return Err(Error::Archive(format!("unsafe archive path {raw:?}")));
        }
    }
    Ok(name.to_owned())
}

fn entry_id(header_val: u64, pax_val: Option<i64>, name: &str) -> Result<u32, Error> {
    let val = match pax_val {
        Some(v) => {
            if !(0..=0x7fff_ffff).contains(&v) {
                return Err(Error::Archive(format!("invalid metadata for {name}")));
            }
            v as u64
        }
        None => header_val,
    };
    if val > 0x7fff_ffff {
        return Err(Error::Archive(format!("invalid metadata for {name}")));
    }
    Ok(val as u32)
}

fn read_pax_int<R: Read>(entry: &mut tar::Entry<'_, R>, key: &str, name: &str) -> Result<Option<i64>, Error> {
    let Some(exts) = entry.pax_extensions()? else {
        return Ok(None);
    };
    for ext in exts {
        let ext = ext?;
        if ext.key() == Ok(key) {
            let text = ext
                .value()
                .map_err(|_| Error::Archive(format!("invalid metadata for {name}")))?;
            let parsed = text
                .parse::<i64>()
                .map_err(|_| Error::Archive(format!("invalid metadata for {name}")))?;
            return Ok(Some(parsed));
        }
    }
    Ok(None)
}

pub fn read_and_validate_archive(
    reader: impl Read,
    expected_sha256: Option<&str>,
) -> Result<BTreeMap<String, TarMember>, Error> {
    let mut hashing = HashingReader::new(reader);
    let mut members = BTreeMap::new();

    {
        let mut archive = tar::Archive::new(&mut hashing);
        for entry in archive.entries()? {
            let mut entry = entry?;
            let raw_name = entry.path_bytes();
            let raw_name = std::str::from_utf8(&raw_name)
                .map_err(|_| Error::Archive("archive member path is not UTF-8".into()))?;
            let name = normalize_archive_name(raw_name)?;
            let etype = entry.header().entry_type();

            if name.is_empty() {
                if !etype.is_dir() {
                    return Err(Error::Archive("archive root is not a directory".into()));
                }
                continue;
            }

            if members.contains_key(&name) {
                return Err(Error::Archive(format!(
                    "duplicate or unsupported archive member {name}"
                )));
            }
            if !(etype.is_dir() || etype.is_file() || etype.is_symlink() || etype.is_hard_link()) {
                return Err(Error::Archive(format!(
                    "duplicate or unsupported archive member {name}"
                )));
            }

            let raw_mode = entry.header().mode()?;
            if raw_mode & !0o7777 != 0 {
                return Err(Error::Archive(format!("invalid metadata for {name}")));
            }
            let pax_uid = read_pax_int(&mut entry, "uid", &name)?;
            let pax_gid = read_pax_int(&mut entry, "gid", &name)?;
            let uid = entry_id(entry.header().uid()?, pax_uid, &name)?;
            let gid = entry_id(entry.header().gid()?, pax_gid, &name)?;
            let mode = raw_mode as u16;

            let kind = if etype.is_dir() {
                TarEntryKind::Dir
            } else if etype.is_file() {
                let mut buf = Vec::with_capacity(entry.size() as usize);
                entry.read_to_end(&mut buf)?;
                TarEntryKind::File(Arc::from(buf))
            } else if etype.is_symlink() {
                let link_bytes = entry
                    .link_name_bytes()
                    .ok_or_else(|| Error::Archive(format!("invalid symlink {name}")))?;
                let target =
                    std::str::from_utf8(&link_bytes).map_err(|_| Error::Archive(format!("invalid symlink {name}")))?;
                if target.is_empty() || target.contains('\0') {
                    return Err(Error::Archive(format!("invalid symlink {name}")));
                }
                TarEntryKind::Symlink(target.to_owned())
            } else {
                let link_bytes = entry
                    .link_name_bytes()
                    .ok_or_else(|| Error::Archive(format!("{name}: hard link missing target")))?;
                let target = std::str::from_utf8(&link_bytes)
                    .map_err(|_| Error::Archive(format!("{name}: hard link target is not UTF-8")))?;
                let normalized_target = normalize_archive_name(target)?;
                TarEntryKind::HardLink(normalized_target)
            };

            members.insert(name, TarMember { uid, gid, mode, kind });
        }
    }

    let got_sha256 = hashing.finish()?;
    if let Some(expected) = expected_sha256
        && got_sha256 != expected
    {
        return Err(Error::Digest("payload.tar SHA-256 differs from its manifest".into()));
    }

    validate_links(&members)?;
    Ok(members)
}

fn validate_links(members: &BTreeMap<String, TarMember>) -> Result<(), Error> {
    for (name, member) in members {
        let mut cursor = name.as_str();
        while let Some((parent, _)) = cursor.rsplit_once('/') {
            if let Some(parent_entry) = members.get(parent)
                && !matches!(parent_entry.kind, TarEntryKind::Dir)
            {
                return Err(Error::Archive(format!("{name} is below non-directory {parent}")));
            }
            cursor = parent;
        }
        if let TarEntryKind::HardLink(target_name) = &member.kind {
            let Some(target) = members.get(target_name) else {
                return Err(Error::Archive(format!(
                    "{name}: hard link must name a regular archive file"
                )));
            };
            if !matches!(target.kind, TarEntryKind::File(_)) {
                return Err(Error::Archive(format!(
                    "{name}: hard link must name a regular archive file"
                )));
            }
            if (member.uid, member.gid, member.mode) != (target.uid, target.gid, target.mode) {
                return Err(Error::Archive(format!("{name}: hard-link metadata differs")));
            }
        }
    }
    Ok(())
}

/// Builds the sorted `metadata.json` entry list per `tools/install/bootstrap.py:_metadata`.
pub fn build_metadata(members: &BTreeMap<String, TarMember>) -> Vec<MetadataEntry> {
    let mut map: BTreeMap<String, MetadataEntry> = BTreeMap::new();
    map.insert(
        String::new(),
        MetadataEntry {
            path: String::new(),
            uid: 0,
            gid: 0,
            mode: 0o755,
            kind: "directory".into(),
        },
    );
    for (name, member) in members {
        let mut cursor = name.as_str();
        while let Some((parent, _)) = cursor.rsplit_once('/') {
            map.entry(parent.to_owned()).or_insert_with(|| MetadataEntry {
                path: parent.to_owned(),
                uid: 0,
                gid: 0,
                mode: 0o755,
                kind: "directory".into(),
            });
            cursor = parent;
        }
        let kind = match &member.kind {
            TarEntryKind::Dir => "directory",
            TarEntryKind::Symlink(_) => "symlink",
            TarEntryKind::File(_) | TarEntryKind::HardLink(_) => "file",
        };
        map.insert(
            name.clone(),
            MetadataEntry {
                path: name.clone(),
                uid: member.uid,
                gid: member.gid,
                mode: member.mode,
                kind: kind.into(),
            },
        );
    }
    map.into_values().collect()
}

pub fn unpacked_bytes(reader: impl Read) -> Result<u64, Error> {
    let members = read_and_validate_archive(reader, None)?;
    Ok(members
        .values()
        .filter_map(|m| match &m.kind {
            TarEntryKind::File(bytes) => Some((bytes.len() as u64).div_ceil(4096) * 4096),
            _ => None,
        })
        .sum())
}
