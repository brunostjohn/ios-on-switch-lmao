// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;

use switch_apfs_format::fs_records::UF_COMPRESSED;
use switch_apfs_read::Fs;
use switch_apfs_write::{Attrs, Tree};
use switch_image::{
    DeltaEntry, DeltaFiles, SF_IMMUTABLE, descendant_dirs, ensure_dir, ensure_dir_under, is_staged_app, split_parent,
    stream_from_replacement,
};

use crate::manifest::{Manifest, ManifestFile};
use crate::overlay_index::{self, CDHASH_LEN, IndexEntry, OriginalsPair};
use crate::{DATA_FOLDER, Error};

#[derive(Clone, Debug)]
pub struct PlacedPretranslated {
    pub manifest: Manifest,
    pub manifest_json: Vec<u8>,
    pub index_bytes: Vec<u8>,
    pub originals_bytes: Option<Vec<u8>>,
    pub placed_files: usize,
    pub hard_links: usize,
    pub directories: usize,
}

struct FirstPlaced {
    path: String,
    sha256_out: [u8; 32],
    cdhash: Vec<String>,
    data_id: u64,
    mode: String,
}

fn immutable_dir_attrs() -> Attrs {
    let mut attrs = Attrs::new(0, 0, 0o755);
    attrs.bsd_flags = SF_IMMUTABLE;
    attrs
}

fn immutable_file_attrs() -> Attrs {
    let mut attrs = Attrs::new(0, 0, 0o644);
    attrs.bsd_flags = SF_IMMUTABLE;
    attrs
}

pub fn parse_cdhash_pairs(text: &str) -> Result<Vec<OriginalsPair>, Error> {
    let mut out = Vec::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let mut words = line.split_whitespace();
        let (Some(old_hex), Some(new_hex), None) = (words.next(), words.next(), words.next()) else {
            return Err(Error::InvalidCdhashPairLine(line.into()));
        };
        let mut original = [0u8; CDHASH_LEN];
        let mut translated = [0u8; CDHASH_LEN];
        hex::decode_to_slice(old_hex, &mut original).map_err(|_| Error::InvalidCdhashPairLine(line.into()))?;
        hex::decode_to_slice(new_hex, &mut translated).map_err(|_| Error::InvalidCdhashPairLine(line.into()))?;
        out.push(OriginalsPair { original, translated });
    }
    Ok(out)
}

fn entry_cdhash(path: &str, cdhash: &[String]) -> Result<[u8; CDHASH_LEN], Error> {
    let Some(first) = cdhash.first() else {
        return Ok([0u8; CDHASH_LEN]);
    };
    let mut out = [0u8; CDHASH_LEN];
    hex::decode_to_slice(first, &mut out).map_err(|_| Error::InvalidCdhash {
        path: path.into(),
        value: first.clone(),
    })?;
    Ok(out)
}

/// Places `entries` (from `switch_image::tree_entries`) into `data` under `Switch/pretranslated/`, writes
/// `Switch/pretranslated.json`, `Switch/pretranslated/.index`, and (when staged apps are present)
/// `Switch/pretranslated/.originals`, and returns the manifest JSON bytes (matching `os.overlay.json`).
pub fn place_pretranslated(
    data: &mut Tree,
    system: &Fs,
    entries: &[DeltaEntry],
    files: &dyn DeltaFiles,
    cdhash_pairs: &[OriginalsPair],
) -> Result<PlacedPretranslated, Error> {
    let switch_dir = ensure_dir(data, DATA_FOLDER, Attrs::new(0, 0, 0o755))?;
    let pt_root = ensure_dir_under(data, switch_dir, "pretranslated", immutable_dir_attrs())?;

    let mut by_system_inode: HashMap<u64, FirstPlaced> = HashMap::new();
    let mut index_entries = Vec::with_capacity(entries.len());
    let mut manifest_files = Vec::with_capacity(entries.len());
    let mut placed_files = 0usize;
    let mut hard_links = 0usize;

    for entry in entries {
        overlay_index::check_path(&entry.path)?;
        let sys_oid = system
            .lookup(&entry.path)?
            .ok_or_else(|| Error::MissingInSource(entry.path.clone()))?;
        let sys_inode = system
            .inode(sys_oid)?
            .ok_or_else(|| Error::MissingInSource(entry.path.clone()))?;
        if !sys_inode.is_regular() {
            return Err(Error::NotRegularFile(entry.path.clone()));
        }

        let cdhash_bytes = entry_cdhash(&entry.path, &entry.cdhash)?;

        let (data_id, mode_str) = if let Some(first) = by_system_inode.get(&sys_oid) {
            if first.sha256_out != entry.sha256_out || first.cdhash != entry.cdhash {
                return Err(Error::ConflictingTranslation {
                    first: first.path.clone(),
                    second: entry.path.clone(),
                });
            }
            hard_links += 1;
            (first.data_id, first.mode.clone())
        } else {
            let replacement = files.load(entry)?;
            let stream = stream_from_replacement(data, &entry.path, replacement, entry.sha256_out)?;
            let (uid, gid) = if is_staged_app(&entry.path) {
                (0, 0)
            } else {
                (sys_inode.val.owner.get(), sys_inode.val.group.get())
            };
            let mode = sys_inode.val.mode.get() & 0o7777;
            let mut attrs = Attrs::new(uid, gid, mode);
            attrs.bsd_flags = (sys_inode.val.bsd_flags.get() & !UF_COMPRESSED) | SF_IMMUTABLE;

            let (parent_parts, name) = split_parent(&entry.path)?;
            let mut parent = pt_root;
            for part in parent_parts {
                parent = ensure_dir_under(data, parent, part, immutable_dir_attrs())?;
            }
            let file_inode = data.add_file(parent, name, attrs, stream)?;
            let data_id = data.number(file_inode);
            let mode_str = format!("{mode:04o}");
            placed_files += 1;
            by_system_inode.insert(
                sys_oid,
                FirstPlaced {
                    path: entry.path.clone(),
                    sha256_out: entry.sha256_out,
                    cdhash: entry.cdhash.clone(),
                    data_id,
                    mode: mode_str.clone(),
                },
            );
            (data_id, mode_str)
        };

        index_entries.push(IndexEntry {
            path: entry.path.clone(),
            system_id: sys_oid,
            data_id,
            cdhash: cdhash_bytes,
        });
        manifest_files.push(ManifestFile {
            path: entry.path.clone(),
            trust: true,
            mode: mode_str,
            sha256: hex::encode(entry.sha256_out),
            cdhash: entry.cdhash.clone(),
        });
    }

    let manifest = Manifest { files: manifest_files };
    let manifest_json = manifest.to_json().into_bytes();
    let manifest_stream = data.add_bytes(manifest_json.clone());
    data.add_file(
        switch_dir,
        "pretranslated.json",
        immutable_file_attrs(),
        Some(manifest_stream),
    )?;

    let dirs = descendant_dirs(data, pt_root)?;
    let mut dir_ids = Vec::with_capacity(dirs.len());
    for dir in dirs {
        let attrs = &mut data.inode_mut(dir).attrs;
        attrs.uid = 0;
        attrs.gid = 0;
        attrs.mode = 0o755;
        attrs.bsd_flags = SF_IMMUTABLE;
        dir_ids.push(data.number(dir));
    }

    let index_bytes = overlay_index::encode(&index_entries, &dir_ids)?;
    let index_stream = data.add_bytes(index_bytes.clone());
    data.add_file(pt_root, overlay_index::NAME, immutable_file_attrs(), Some(index_stream))?;

    let staged_cdhashes: Vec<[u8; CDHASH_LEN]> = index_entries
        .iter()
        .filter(|e| is_staged_app(&e.path))
        .map(|e| e.cdhash)
        .collect();
    let originals_bytes = if staged_cdhashes.is_empty() {
        None
    } else {
        let mut original_of: HashMap<[u8; CDHASH_LEN], [u8; CDHASH_LEN]> = HashMap::new();
        for pair in cdhash_pairs {
            match original_of.get(&pair.translated) {
                Some(&prev) if prev != pair.original => {
                    return Err(Error::ConflictingCdhashPair {
                        translated: hex::encode(pair.translated),
                    });
                }
                None => {
                    original_of.insert(pair.translated, pair.original);
                }
                Some(_) => {}
            }
        }
        let mut staged_pairs = Vec::with_capacity(staged_cdhashes.len());
        for cdhash in staged_cdhashes {
            let original = original_of
                .get(&cdhash)
                .copied()
                .ok_or_else(|| Error::MissingOriginalCdhash(hex::encode(cdhash)))?;
            staged_pairs.push(OriginalsPair {
                original,
                translated: cdhash,
            });
        }
        let raw = overlay_index::encode_originals(&staged_pairs)?;
        let originals_stream = data.add_bytes(raw.clone());
        data.add_file(
            pt_root,
            overlay_index::ORIGINALS_NAME,
            immutable_file_attrs(),
            Some(originals_stream),
        )?;
        Some(raw)
    };

    Ok(PlacedPretranslated {
        manifest,
        manifest_json,
        index_bytes,
        originals_bytes,
        placed_files,
        hard_links,
        directories: dir_ids.len(),
    })
}
