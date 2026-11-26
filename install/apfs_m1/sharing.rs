// SPDX-License-Identifier: GPL-3.0-or-later
//! Which inodes of a walked subtree are clones of each other (they share a private id), and the clone-mapping record
//! each has, so `fidelity` can compare a copy's sharing with its source's by path.

use std::collections::{BTreeMap, BTreeSet};

use switch_apfs_format::clone_mapping::{
    APFS_ROCOMPAT_CLONE_MAPPING, APFS_TYPE_CLONE_MAPPING, CLONE_MAPPING_OID, CloneMappingKey, CloneMappingVal,
};
use switch_apfs_format::get;
use switch_apfs_read::{Entry, Fs};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

pub struct Sharing {
    /// Per path, one path for each inode sharing its private id, itself included.
    groups: BTreeMap<String, Vec<String>>,
    /// Per path, its clone mapping's physical size; None on a volume without clone mappings.
    mappings: Option<BTreeMap<String, u64>>,
    /// Per path, how many inodes the volume maps under its private id, the subtree's or not.
    mapped: BTreeMap<String, usize>,
}

impl Sharing {
    pub fn read(fs: &Fs, entries: &BTreeMap<String, Entry>) -> Result<Self> {
        let mut first_path: BTreeMap<u64, &str> = BTreeMap::new();
        for (path, e) in entries.iter().filter(|(p, _)| p.as_str() != "/") {
            first_path.entry(e.inode.oid).or_insert(path);
        }
        let mut by_private: BTreeMap<u64, BTreeSet<&str>> = BTreeMap::new();
        for &path in first_path.values() {
            let private = entries[path].inode.val.private_id.get();
            by_private.entry(private).or_default().insert(path);
        }
        let mut records = BTreeMap::new();
        let mut per_private: BTreeMap<u64, usize> = BTreeMap::new();
        for r in fs.object(CLONE_MAPPING_OID, Some(APFS_TYPE_CLONE_MAPPING))? {
            let k: CloneMappingKey = get(&r.key, 0).ok_or("a short clone-mapping key")?;
            let v: CloneMappingVal = get(&r.value, 0).ok_or("a short clone-mapping value")?;
            records.insert((k.private_id.get(), k.file_id.get()), v.physical_size.get());
            *per_private.entry(k.private_id.get()).or_default() += 1;
        }
        let has_mappings = fs.sb.readonly_compatible_features.get() & APFS_ROCOMPAT_CLONE_MAPPING != 0;
        let mut out = Sharing {
            groups: BTreeMap::new(),
            mappings: has_mappings.then(BTreeMap::new),
            mapped: BTreeMap::new(),
        };
        for (path, e) in entries.iter().filter(|(p, _)| p.as_str() != "/") {
            let private = e.inode.val.private_id.get();
            let group = by_private[&private].iter().map(|p| p.to_string()).collect();
            out.groups.insert(path.clone(), group);
            out.mapped
                .insert(path.clone(), per_private.get(&private).copied().unwrap_or(0));
            if let (Some(m), Some(&size)) = (out.mappings.as_mut(), records.get(&(private, e.inode.oid))) {
                m.insert(path.clone(), size);
            }
        }
        Ok(out)
    }

    pub fn group(&self, path: &str) -> &[String] {
        self.groups.get(path).map_or(&[], |g| &g[..])
    }

    /// None when the volume has no clone mappings at all.
    pub fn mapping(&self, path: &str) -> Option<Option<u64>> {
        self.mappings.as_ref().map(|m| m.get(path).copied())
    }

    /// The volume maps more inodes under this path's private id than the subtree holds.
    pub fn mapped_outside(&self, path: &str) -> bool {
        self.mapped.get(path).copied().unwrap_or(0) > self.group(path).len()
    }
}
