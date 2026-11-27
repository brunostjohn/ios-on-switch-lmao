// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::Path;

use sha2::{Digest, Sha256};
use switch_apfs_alloc::Geometry;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME};
use switch_apfs_format::io::{BlockSource, FileSource};
use switch_apfs_format::spaceman::{SD_MAIN, SFQ_IP, SFQ_MAIN};
use switch_apfs_format::volume::{
    APFS_INCOMPAT_EXPANDED_RECORDS, APFS_VOL_ROLE_DATA, APFS_VOL_ROLE_PREBOOT, APFS_VOL_ROLE_SYSTEM,
};
use switch_apfs_read::{Container, Entry, Fs, Step, Volume, stream};
use switch_image::{DATA_ROCOMPAT_FEATURES, SF_IMMUTABLE, STAGING_DIR, is_staged_app};
use switch_image_pretranslated::{OVERLAY_DIR, OVERLAY_INDEX, OVERLAY_ORIGINALS, overlay_index};

use crate::parse::uuid_string;
use crate::{Error, Result};

#[derive(Debug)]
pub struct PrebootExtentInfo {
    pub name: String,
    pub size: u64,
    pub paddr: u64,
    pub blocks: u64,
    pub sha256: Option<String>,
}

pub fn verify_preboot_extents(
    image: &Path,
    offset: u64,
    preboot_uuid: Option<[u8; 16]>,
    extract_dir: Option<&Path>,
    expect_count: Option<usize>,
) -> Result<Vec<PrebootExtentInfo>> {
    let src = FileSource::open(image, offset)?;
    let container = Container::open(&src)?;
    let vol = pick_volume(&container, preboot_uuid, APFS_VOL_ROLE_PREBOOT, "Preboot")?;
    let fs = Fs::open(&src, vol)?;

    let staging = format!("/{STAGING_DIR}");
    if fs.lookup(&staging)?.is_none() {
        return Err(Error::Check(format!("Preboot missing {staging}")));
    }

    if let Some(dir) = extract_dir {
        fs::create_dir_all(dir)?;
    }

    let mut found = Vec::new();
    let mut check_err: Option<Error> = None;
    fs.walk(&staging, &mut |entry| {
        if entry.path == staging {
            return Ok(Step::Into);
        }
        if !entry.inode.is_regular() {
            check_err = Some(Error::Check(format!("{}: expected regular file", entry.path)));
            return Ok(Step::Over);
        }
        match check_and_extract_cryptex(&src, entry, extract_dir) {
            Ok(info) => found.push(info),
            Err(e) => check_err = Some(e),
        }
        Ok(Step::Over)
    })?;

    if let Some(err) = check_err {
        return Err(err);
    }
    if found.is_empty() {
        return Err(Error::Check(format!("no cryptex images under {staging}")));
    }
    if let Some(want) = expect_count
        && found.len() != want
    {
        return Err(Error::Check(format!(
            "expected {want} cryptex images under {staging}, found {}",
            found.len()
        )));
    }
    Ok(found)
}

fn check_and_extract_cryptex(
    src: &dyn BlockSource,
    entry: &Entry,
    extract_dir: Option<&Path>,
) -> Result<PrebootExtentInfo> {
    if entry.inode.is_compressed() {
        return Err(Error::Check(format!("{}: has UF_COMPRESSED", entry.path)));
    }
    if entry.xattr(DECMPFS_XATTR_NAME).is_some() || entry.xattr(RESOURCE_FORK_XATTR_NAME).is_some() {
        return Err(Error::Check(format!("{}: carries compression xattrs", entry.path)));
    }
    let Some(s) = &entry.data else {
        return Err(Error::Check(format!("{}: empty cryptex image", entry.path)));
    };
    if s.size == 0 {
        return Err(Error::Check(format!("{}: zero-byte cryptex image", entry.path)));
    }
    if s.extents.len() != 1 {
        return Err(Error::Check(format!(
            "{}: expected 1 contiguous extent, found {}",
            entry.path,
            s.extents.len()
        )));
    }
    let ext = s.extents[0];
    let want_len = s.size.next_multiple_of(BLOCK_SIZE as u64);
    if ext.logical_addr != 0 || ext.phys_block_num == 0 || ext.len != want_len {
        return Err(Error::Check(format!(
            "{}: bad extent logical={} paddr={} len={} (want len={want_len})",
            entry.path, ext.logical_addr, ext.phys_block_num, ext.len
        )));
    }

    let name = entry
        .path
        .rsplit_once('/')
        .map_or(entry.path.as_str(), |(_, n)| n)
        .to_owned();

    let sha256 = if let Some(dir) = extract_dir {
        let out_path = dir.join(&name);
        let mut writer = BufWriter::with_capacity(1 << 20, File::create(&out_path)?);
        let mut hasher = Sha256::new();
        let mut write_err: Option<std::io::Error> = None;
        stream::read(src, s, &mut |chunk| {
            if write_err.is_none() {
                hasher.update(chunk);
                if let Err(e) = writer.write_all(chunk) {
                    write_err = Some(e);
                }
            }
        })?;
        if let Some(e) = write_err {
            return Err(e.into());
        }
        writer.flush()?;
        Some(hex::encode(hasher.finalize()))
    } else {
        None
    };

    Ok(PrebootExtentInfo {
        name,
        size: s.size,
        paddr: ext.phys_block_num,
        blocks: ext.blocks(),
        sha256,
    })
}

#[derive(Debug)]
pub struct IndexVerifyReport {
    pub files: usize,
    pub dirs: usize,
    pub originals: usize,
}

pub fn verify_index(
    image: &Path,
    offset: u64,
    system_uuid: Option<[u8; 16]>,
    data_uuid: Option<[u8; 16]>,
    expect_originals: bool,
    dump_tsv: Option<&Path>,
) -> Result<IndexVerifyReport> {
    let src = FileSource::open(image, offset)?;
    let container = Container::open(&src)?;
    let sys_vol = pick_volume(&container, system_uuid, APFS_VOL_ROLE_SYSTEM, "System")?;
    let data_vol = pick_volume(&container, data_uuid, APFS_VOL_ROLE_DATA, "Data")?;
    let sys_fs = Fs::open(&src, sys_vol)?;
    let data_fs = Fs::open(&src, data_vol)?;

    let idx_path = format!("/{OVERLAY_INDEX}");
    let raw_idx = read_immutable_file(&src, &data_fs, &idx_path)?;
    let (entries, dirs) = overlay_index::decode(&raw_idx)?;

    let mut by_sys: BTreeMap<u64, Vec<&overlay_index::IndexEntry>> = BTreeMap::new();
    for entry in &entries {
        let sys_oid = sys_fs
            .lookup(&entry.path)?
            .ok_or_else(|| Error::Check(format!("index path {} missing on System", entry.path)))?;
        if sys_oid != entry.system_id {
            return Err(Error::Check(format!(
                "{}: index system_id {} != System inode {sys_oid}",
                entry.path, entry.system_id
            )));
        }
        let sys_inode = sys_fs
            .inode(sys_oid)?
            .ok_or_else(|| Error::Check(format!("System inode {sys_oid} missing")))?;
        if !sys_inode.is_regular() {
            return Err(Error::Check(format!("{}: not regular file on System", entry.path)));
        }
        by_sys.entry(entry.system_id).or_default().push(entry);
    }

    for (sys_id, group) in &by_sys {
        let want_data_id = group[0].data_id;
        let data_inode = data_fs
            .inode(want_data_id)?
            .ok_or_else(|| Error::Check(format!("data_id {want_data_id} missing on Data")))?;
        if !data_inode.is_regular() || data_inode.is_compressed() || data_inode.val.bsd_flags.get() & SF_IMMUTABLE == 0
        {
            return Err(Error::Check(format!(
                "data_id {want_data_id}: must be regular, uncompressed, SF_IMMUTABLE"
            )));
        }
        let mut matched_path = 0usize;
        for item in group {
            let placed_path = format!("/{OVERLAY_DIR}{}", item.path);
            if let Some(got_oid) = data_fs.lookup(&placed_path)? {
                if got_oid != want_data_id {
                    return Err(Error::Check(format!(
                        "{placed_path}: Data inode {got_oid} != index data_id {want_data_id}"
                    )));
                }
                matched_path += 1;
            }
        }
        if matched_path == 0 {
            return Err(Error::Check(format!(
                "System inode {sys_id}: no placed file found under /{OVERLAY_DIR}"
            )));
        }
    }

    let pt_root = format!("/{OVERLAY_DIR}");
    let mut actual_dirs = Vec::new();
    let mut dir_err: Option<Error> = None;
    data_fs.walk(&pt_root, &mut |entry| {
        if entry.inode.is_dir() {
            let v = &entry.inode.val;
            if v.owner.get() != 0
                || v.group.get() != 0
                || (v.mode.get() & 0o7777) != 0o755
                || v.bsd_flags.get() & SF_IMMUTABLE == 0
            {
                dir_err = Some(Error::Check(format!(
                    "{}: pretranslated dir must be 0:0 0755 SF_IMMUTABLE",
                    entry.path
                )));
                return Ok(Step::Over);
            }
            actual_dirs.push(entry.inode.oid);
        }
        Ok(Step::Into)
    })?;
    if let Some(err) = dir_err {
        return Err(err);
    }
    actual_dirs.sort_unstable();
    if actual_dirs != dirs {
        return Err(Error::Check(format!(
            ".index dir ids {dirs:?} != Data dirs {actual_dirs:?}"
        )));
    }

    let orig_path = format!("/{OVERLAY_ORIGINALS}");
    let originals = if data_fs.lookup(&orig_path)?.is_some() {
        let raw_orig = read_immutable_file(&src, &data_fs, &orig_path)?;
        overlay_index::decode_originals(&raw_orig)?
    } else if expect_originals {
        return Err(Error::Check(format!("missing {orig_path}")));
    } else {
        Vec::new()
    };

    let translated_set: BTreeSet<[u8; overlay_index::CDHASH_LEN]> = originals.iter().map(|p| p.translated).collect();
    for entry in &entries {
        if is_staged_app(&entry.path) && !translated_set.contains(&entry.cdhash) {
            return Err(Error::Check(format!(
                "staged app {} cdhash {} missing from .originals",
                entry.path,
                hex::encode(entry.cdhash)
            )));
        }
    }

    if let Some(tsv_path) = dump_tsv {
        let mut out = String::new();
        for f in &entries {
            out.push_str(&format!(
                "file\t{}\t{}\t{}\t{}\n",
                f.path,
                f.system_id,
                f.data_id,
                hex::encode(f.cdhash)
            ));
        }
        for d in &dirs {
            out.push_str(&format!("dir\t{d}\n"));
        }
        for p in &originals {
            out.push_str(&format!(
                "orig\t{}\t{}\n",
                hex::encode(p.original),
                hex::encode(p.translated)
            ));
        }
        fs::write(tsv_path, out)?;
    }

    Ok(IndexVerifyReport {
        files: entries.len(),
        dirs: dirs.len(),
        originals: originals.len(),
    })
}

pub struct PinExpectation<'a> {
    pub system_uuid: [u8; 16],
    pub data_uuid: [u8; 16],
    pub data_name: &'a str,
    pub preboot_uuid: [u8; 16],
    pub preboot_name: &'a str,
    pub expected_blocks: Option<u64>,
}

pub fn verify_pins(image: &Path, offset: u64, want: &PinExpectation<'_>) -> Result<()> {
    let src = FileSource::open(image, offset)?;
    let c = Container::open(&src)?;
    if let Some(blocks) = want.expected_blocks
        && c.blocks() != blocks
    {
        return Err(Error::Check(format!(
            "container blocks {} != expected {blocks}",
            c.blocks()
        )));
    }
    if c.volumes.len() != 3 {
        return Err(Error::Check(format!("expected 3 volumes, got {}", c.volumes.len())));
    }

    let [sys, data, boot] = c.volumes.as_slice() else {
        unreachable!("checked len == 3");
    };
    check_vol(sys, APFS_VOL_ROLE_SYSTEM, want.system_uuid, None)?;
    check_vol(data, APFS_VOL_ROLE_DATA, want.data_uuid, Some(want.data_name))?;
    check_vol(boot, APFS_VOL_ROLE_PREBOOT, want.preboot_uuid, Some(want.preboot_name))?;

    if data.sb.readonly_compatible_features.get() != DATA_ROCOMPAT_FEATURES {
        return Err(Error::Check(format!(
            "Data rocompat {:#x} != {DATA_ROCOMPAT_FEATURES:#x}",
            data.sb.readonly_compatible_features.get()
        )));
    }
    if data.sb.incompatible_features.get() & APFS_INCOMPAT_EXPANDED_RECORDS == 0 {
        return Err(Error::Check(format!(
            "Data incompat {:#x} missing APFS_INCOMPAT_EXPANDED_RECORDS",
            data.sb.incompatible_features.get()
        )));
    }
    if boot.sb.readonly_compatible_features.get() != 0
        || boot.sb.incompatible_features.get() & APFS_INCOMPAT_EXPANDED_RECORDS != 0
    {
        return Err(Error::Check("Preboot feature bits differ from expected".into()));
    }

    let g = Geometry::for_blocks(c.blocks())?;
    let sm = &c.space.spaceman;
    if sm.dev[SD_MAIN].chunk_count.get() != g.chunks
        || u64::from(sm.dev[SD_MAIN].cib_count.get()) != g.cibs
        || sm.ip_block_count.get() != g.pool_blocks
        || sm.fq[SFQ_IP].tree_node_limit.get() != g.pool_queue_limit
        || sm.fq[SFQ_MAIN].tree_node_limit.get() != g.main_queue_limit
        || c.checkpoint.nx.max_file_systems.get() != g.max_file_systems
    {
        return Err(Error::Check(
            "spaceman geometry disagrees with Geometry::for_blocks".into(),
        ));
    }

    println!(
        "geometry {} {} {} {} {} {} {} {}",
        g.blocks,
        sm.ip_base.get(),
        sm.ip_block_count.get(),
        g.chunks,
        g.cibs,
        g.main_queue_limit,
        g.pool_queue_limit,
        g.max_file_systems,
    );
    Ok(())
}

fn check_vol(v: &Volume, role: u16, uuid: [u8; 16], name: Option<&str>) -> Result<()> {
    if v.sb.role.get() != role {
        return Err(Error::Check(format!(
            "volume {} role {:#x} != {role:#x}",
            v.index,
            v.sb.role.get()
        )));
    }
    if v.sb.vol_uuid != uuid {
        return Err(Error::Check(format!(
            "volume {} UUID {} != {}",
            v.index,
            uuid_string(&v.sb.vol_uuid),
            uuid_string(&uuid)
        )));
    }
    if let Some(want_name) = name
        && v.sb.name() != want_name
    {
        return Err(Error::Check(format!(
            "volume {} name {:?} != {want_name:?}",
            v.index,
            v.sb.name()
        )));
    }
    Ok(())
}

fn pick_volume<'c>(c: &'c Container, uuid: Option<[u8; 16]>, role: u16, label: &str) -> Result<&'c Volume> {
    let found = match uuid {
        Some(u) => c.volume(u),
        None => c.volumes.iter().find(|v| v.sb.role.get() == role),
    };
    found.ok_or_else(|| Error::Check(format!("container has no {label} volume")))
}

fn read_immutable_file(src: &dyn BlockSource, fs: &Fs, path: &str) -> Result<Vec<u8>> {
    let oid = fs
        .lookup(path)?
        .ok_or_else(|| Error::Check(format!("missing {path}")))?;
    let (entry, _) = fs.entry(oid, path.to_owned(), None)?;
    let v = &entry.inode.val;
    if !entry.inode.is_regular()
        || v.owner.get() != 0
        || v.group.get() != 0
        || (v.mode.get() & 0o7777) != 0o644
        || v.bsd_flags.get() & SF_IMMUTABLE == 0
    {
        return Err(Error::Check(format!("{path}: must be regular 0:0 0644 SF_IMMUTABLE")));
    }
    let Some(s) = &entry.data else {
        return Ok(Vec::new());
    };
    let mut raw = Vec::with_capacity(s.size as usize);
    stream::read(src, s, &mut |chunk| raw.extend_from_slice(chunk))?;
    Ok(raw)
}
