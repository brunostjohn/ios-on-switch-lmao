// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;

use sha2::{Digest, Sha256};
use switch_apfs_format::clone_mapping::APFS_TYPE_CLONE_MAPPING;
use switch_apfs_format::container::NxSuperblock;
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE, APFS_TYPE_XATTR,
    INO_EXT_TYPE_DSTREAM, ROOT_DIR_INO_NUM,
};
use switch_apfs_format::fs_records::{
    APFS_FILE_INFO_DATA_HASH, APFS_HASH_SHA256_SIZE, APFS_TYPE_DIR_STATS, APFS_TYPE_FILE_INFO, APFS_TYPE_SIBLING_LINK,
    APFS_TYPE_SIBLING_MAP, DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::volume::{APFS_INCOMPAT_CASE_INSENSITIVE, ApfsSuperblock};
use switch_apfs_format::{BLOCK_SIZE, Block};
use switch_apfs_read::{
    Container, DirRec, FileExtent, FileInfo, Fs, Inode, Record, SiblingLink, Stream, Xattr, XattrData, catalog, stream,
};
use unicode_normalization::UnicodeNormalization;

use crate::SealError;

const MAX_XATTR_PREALLOC: u64 = 16 << 20;

pub struct SourceCryptex {
    pub nx: NxSuperblock,
    pub sb: ApfsSuperblock,
    pub sb_block: Box<Block>,
    pub raw_records: Vec<(Vec<u8>, Vec<u8>)>,
    pub inodes: HashMap<u64, Inode>,
    pub xattrs: HashMap<u64, Vec<Xattr>>,
    pub children: HashMap<u64, Vec<DirRec>>,
    pub fext: HashMap<u64, Vec<FileExtent>>,
    pub max_fs_oid: u64,
}

impl SourceCryptex {
    pub fn read(src: &dyn BlockSource) -> Result<Self, SealError> {
        let container = Container::open(src)?;
        let [vol] = container.volumes.as_slice() else {
            return Err(SealError::InvalidSource(format!(
                "expected 1 volume, found {}",
                container.volumes.len()
            )));
        };
        let fs = Fs::open(src, vol)?;
        if fs.seal.is_none() {
            return Err(SealError::InvalidSource("source volume is not sealed".into()));
        }
        let sb = fs.sb;
        if sb.num_snapshots.get() != 0
            || sb.snap_meta_ext_oid.get() != 0
            || fs.omap.phys.snap_count.get() != 0
            || container.omap.phys.snap_count.get() != 0
        {
            return Err(SealError::InvalidSource("source volume has snapshots".into()));
        }
        if !fs.hashed_names() || sb.incompatible_features.get() & APFS_INCOMPAT_CASE_INSENSITIVE != 0 {
            return Err(SealError::InvalidSource(format!(
                "unsupported volume incompatible features {:#x}",
                sb.incompatible_features.get()
            )));
        }

        let mut raw_records = Vec::new();
        let mut inodes = HashMap::new();
        let mut xattrs: HashMap<u64, Vec<Xattr>> = HashMap::new();
        let mut children: HashMap<u64, Vec<DirRec>> = HashMap::new();
        let mut max_fs_oid = sb.next_obj_id.get();
        let mut scan_err: Option<SealError> = None;

        let res = fs.records(&mut |key, value| {
            let (oid, kind) = catalog::key_header(key)?;
            max_fs_oid = max_fs_oid.max(oid);
            match kind {
                APFS_TYPE_INODE => {
                    let inode = Inode::decode(oid, value)?;
                    if inode.xfield(INO_EXT_TYPE_DSTREAM).is_some() && inode.dstream().is_none() {
                        scan_err = Some(SealError::InvalidSource(format!(
                            "inode {oid}: malformed dstream xfield"
                        )));
                        return Err(switch_apfs_read::ReadError::invalid("malformed dstream xfield"));
                    }
                    max_fs_oid = max_fs_oid.max(inode.val.private_id.get());
                    if inodes.insert(oid, inode).is_some() {
                        scan_err = Some(SealError::InvalidSource(format!("duplicate inode {oid}")));
                        return Err(switch_apfs_read::ReadError::invalid("duplicate inode"));
                    }
                }
                APFS_TYPE_DIR_REC => {
                    let drec = DirRec::decode(key, value, true)?;
                    if let Some(sib) = drec.sibling_id() {
                        max_fs_oid = max_fs_oid.max(sib);
                    }
                    children.entry(drec.parent).or_default().push(drec);
                }
                APFS_TYPE_XATTR => {
                    let x = Xattr::decode(key, value)?;
                    if let XattrData::Stream { id, .. } = &x.data {
                        max_fs_oid = max_fs_oid.max(*id);
                    }
                    xattrs.entry(oid).or_default().push(x);
                }
                APFS_TYPE_SIBLING_LINK => {
                    let link = SiblingLink::decode(key, value)?;
                    max_fs_oid = max_fs_oid.max(link.sibling_id);
                }
                APFS_TYPE_SIBLING_MAP => {
                    let Record::SiblingMap { .. } = Record::decode(key, value, true)? else {
                        unreachable!();
                    };
                }
                APFS_TYPE_DSTREAM_ID => {
                    let Record::DstreamId { .. } = Record::decode(key, value, true)? else {
                        unreachable!();
                    };
                }
                APFS_TYPE_FILE_INFO => {
                    let info = FileInfo::decode(key, value)?;
                    if info.info_type != APFS_FILE_INFO_DATA_HASH || info.hash.len() != APFS_HASH_SHA256_SIZE {
                        scan_err = Some(SealError::InvalidSource(format!(
                            "oid {oid}: unsupported FILE_INFO type={} hash_size={}",
                            info.info_type,
                            info.hash.len()
                        )));
                        return Err(switch_apfs_read::ReadError::invalid("unsupported FILE_INFO"));
                    }
                }
                APFS_TYPE_FILE_EXTENT => {
                    scan_err = Some(SealError::InvalidSource(format!(
                        "sealed fs tree contains FILE_EXTENT for oid {oid}"
                    )));
                    return Err(switch_apfs_read::ReadError::invalid("FILE_EXTENT in sealed fs tree"));
                }
                // Apple sealed cryptexes omit DIR_STATS and CLONE_MAPPING; copying them verbatim across
                // file replacements would leave cached sizes or clone maps stale.
                APFS_TYPE_DIR_STATS | APFS_TYPE_CLONE_MAPPING => {
                    scan_err = Some(SealError::InvalidSource(format!(
                        "unsupported fs-tree record type {kind} for oid {oid} in sealed cryptex"
                    )));
                    return Err(switch_apfs_read::ReadError::invalid("unsupported record type"));
                }
                other => {
                    scan_err = Some(SealError::InvalidSource(format!(
                        "unsupported fs-tree record type {other} for oid {oid}"
                    )));
                    return Err(switch_apfs_read::ReadError::invalid("unsupported record type"));
                }
            }
            raw_records.push((key.to_vec(), value.to_vec()));
            Ok(())
        });
        if let Some(err) = scan_err {
            return Err(err);
        }
        res?;

        let container_blocks = container.blocks();
        let mut fext: HashMap<u64, Vec<FileExtent>> = HashMap::new();
        let mut fext_err: Option<SealError> = None;
        let fres = fs.fext_scan(None, &mut |key, value| {
            let val = value.ok_or_else(|| switch_apfs_read::ReadError::invalid("ghost in fext tree"))?;
            let ext = FileExtent::decode_fext(key, val)?;
            let bs = BLOCK_SIZE as u64;
            let phys_end = ext.phys_block_num.checked_add(ext.blocks());
            if ext.phys_block_num == 0
                || ext.len == 0
                || !ext.len.is_multiple_of(bs)
                || !ext.logical_addr.is_multiple_of(bs)
                || phys_end.is_none_or(|end| end > container_blocks)
            {
                fext_err = Some(SealError::InvalidSource(format!(
                    "stream {}: unsupported fext extent logical={} len={} phys={} (container {} blocks)",
                    ext.stream, ext.logical_addr, ext.len, ext.phys_block_num, container_blocks
                )));
                return Err(switch_apfs_read::ReadError::invalid("invalid fext extent"));
            }
            fext.entry(ext.stream).or_default().push(ext);
            Ok(())
        });
        if let Some(err) = fext_err {
            return Err(err);
        }
        fres?;

        for (&sid, extents) in &mut fext {
            extents.sort_by_key(|e| e.logical_addr);
            let mut at = 0u64;
            for e in extents.iter() {
                if e.logical_addr != at {
                    return Err(SealError::InvalidSource(format!(
                        "stream {sid}: non-contiguous logical extents (expected {at}, got {})",
                        e.logical_addr
                    )));
                }
                at = at
                    .checked_add(e.len)
                    .ok_or_else(|| SealError::InvalidSource(format!("stream {sid}: extent length overflow")))?;
            }
        }

        Ok(Self {
            nx: container.checkpoint.nx,
            sb,
            sb_block: vol.block.clone(),
            raw_records,
            inodes,
            xattrs,
            children,
            fext,
            max_fs_oid,
        })
    }

    pub fn lookup(&self, path: &str) -> Result<Option<u64>, SealError> {
        let parts = split_volume_path(path)?;
        let mut oid = ROOT_DIR_INO_NUM;
        for part in parts {
            let Some(entries) = self.children.get(&oid) else {
                return Ok(None);
            };
            let Some(found) = entries.iter().find(|d| d.name.nfd().eq(part.nfd())) else {
                return Ok(None);
            };
            oid = found.file_id();
        }
        Ok(Some(oid))
    }

    pub fn plaintext_sha256(&self, src: &dyn BlockSource, oid: u64, path: &str) -> Result<[u8; 32], SealError> {
        let inode = self
            .inodes
            .get(&oid)
            .ok_or_else(|| SealError::InvalidSource(format!("{path}: missing inode {oid}")))?;
        if inode.is_compressed() {
            let xs = self.xattrs.get(&oid).map_or(&[][..], Vec::as_slice);
            let decmpfs = xs
                .iter()
                .find(|x| x.name == DECMPFS_XATTR_NAME)
                .ok_or_else(|| SealError::InvalidSource(format!("{path}: compressed file has no decmpfs xattr")))?;
            let decmpfs_bytes = self.read_xattr(src, decmpfs, path)?;
            let fork_bytes = match xs.iter().find(|x| x.name == RESOURCE_FORK_XATTR_NAME) {
                Some(fork) => Some(self.read_xattr(src, fork, path)?),
                None => None,
            };
            let plain = switch_decmpfs::decompress(&decmpfs_bytes, fork_bytes.as_deref())?;
            return Ok(Sha256::digest(&plain).into());
        }
        let Some(dstream) = inode.dstream() else {
            return Ok(Sha256::digest(b"").into());
        };
        let size = dstream.size.get();
        if size == 0 {
            return Ok(Sha256::digest(b"").into());
        }
        let sid = inode.val.private_id.get();
        let extents = self
            .fext
            .get(&sid)
            .cloned()
            .ok_or_else(|| SealError::InvalidSource(format!("{path}: stream {sid} has no fext extents")))?;
        let (_, covered) = extent_bytes(sid, &extents)?;
        if covered < size {
            return Err(SealError::InvalidSource(format!(
                "{path}: stream {sid} covers {covered} bytes, less than {size}"
            )));
        }
        let s = Stream {
            id: sid,
            size,
            extents,
            file_info: Vec::new(),
            refcnt: None,
        };
        let mut hasher = Sha256::new();
        stream::read(src, &s, &mut |chunk| hasher.update(chunk))?;
        Ok(hasher.finalize().into())
    }

    fn read_xattr(&self, src: &dyn BlockSource, xattr: &Xattr, path: &str) -> Result<Vec<u8>, SealError> {
        match &xattr.data {
            XattrData::Embedded(bytes) => Ok(bytes.clone()),
            XattrData::Stream { id, dstream } => {
                let size = dstream.dstream.size.get();
                if size == 0 {
                    return Ok(Vec::new());
                }
                let extents = self.fext.get(id).cloned().ok_or_else(|| {
                    SealError::InvalidSource(format!("{path}: xattr {} stream {id} has no extents", xattr.name))
                })?;
                let (_, covered) = extent_bytes(*id, &extents)?;
                if covered < size {
                    return Err(SealError::InvalidSource(format!(
                        "{path}: xattr {} stream {id} covers {covered} bytes, less than {size}",
                        xattr.name
                    )));
                }
                let s = Stream {
                    id: *id,
                    size,
                    extents,
                    file_info: Vec::new(),
                    refcnt: None,
                };
                let cap = usize::try_from(size.min(covered).min(MAX_XATTR_PREALLOC)).unwrap_or(0);
                let mut out = Vec::with_capacity(cap);
                stream::read(src, &s, &mut |chunk| out.extend_from_slice(chunk))?;
                Ok(out)
            }
        }
    }
}

pub fn extent_bytes(sid: u64, extents: &[FileExtent]) -> Result<(u64, u64), SealError> {
    let mut blocks = 0u64;
    for e in extents {
        blocks = blocks
            .checked_add(e.blocks())
            .ok_or_else(|| SealError::InvalidSource(format!("stream {sid}: block count overflow")))?;
    }
    let bytes = blocks
        .checked_mul(BLOCK_SIZE as u64)
        .ok_or_else(|| SealError::InvalidSource(format!("stream {sid}: byte length overflow")))?;
    Ok((blocks, bytes))
}

pub fn split_volume_path(path: &str) -> Result<Vec<&str>, SealError> {
    if !path.starts_with('/') {
        return Err(SealError::InvalidPath(format!(
            "{path:?} must be an absolute volume path"
        )));
    }
    let parts: Vec<&str> = path.split('/').skip(1).collect();
    if parts.is_empty() || parts.iter().any(|p| p.is_empty() || *p == "." || *p == "..") {
        return Err(SealError::InvalidPath(format!(
            "{path:?} is not a normalized file path"
        )));
    }
    Ok(parts)
}
