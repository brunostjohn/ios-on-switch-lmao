// SPDX-License-Identifier: GPL-3.0-or-later
use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap, HashSet};

use switch_apfs_btree::Record;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_INODE, APFS_TYPE_XATTR, Dstream, DstreamIdVal,
    INO_EXT_TYPE_DSTREAM, INODE_NO_RSRC_FORK, JKey, MIN_USER_INO_NUM, PRIV_DIR_INO_NUM, ROOT_DIR_INO_NUM, S_IFDIR,
    S_IFREG, XF_SYSTEM_FIELD,
};
use switch_apfs_format::fs_records::{
    APFS_TYPE_FILE_INFO, APFS_TYPE_SIBLING_LINK, APFS_TYPE_SIBLING_MAP, DECMPFS_XATTR_NAME, INO_EXT_TYPE_SPARSE_BYTES,
    RESOURCE_FORK_XATTR_NAME, S_IFLNK, S_IFMT, SYMLINK_XATTR_NAME, UF_COMPRESSED, XATTR_DATA_STREAM, XattrDstream,
    XattrVal,
};
use switch_apfs_read::{FileExtent, Inode, XField, Xattr, XattrData, catalog};
use switch_apfs_write::inode::{
    INODE_HAS_RSRC_FORK, INODE_HAS_SECURITY_EA, SECURITY_XATTR_NAME, replaced_flags, xfields,
};
pub use switch_apfs_write::keys::{fs_key_order, sort as sort_fs_records};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U32, U64};

use crate::delta::ResolvedReplacement;
use crate::file_info;
use crate::source::{SourceCryptex, extent_bytes};
use crate::{ReplacementData, SealError};

const XATTR_SETXATTR_STREAM: u16 = 0x10;

#[derive(Clone, Debug)]
pub enum StreamSource {
    Source(Vec<FileExtent>),
    Replacement {
        data: ReplacementData,
        size: u64,
        sha256: [u8; 32],
    },
}

#[derive(Clone, Debug)]
pub struct PlannedStream {
    pub stream_id: u64,
    pub owner_oid: u64,
    pub blocks: u64,
    pub source: StreamSource,
}

pub struct BuiltRecords {
    pub fs: Vec<Record>,
    pub streams: Vec<PlannedStream>,
    pub next_obj_id: u64,
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub other: u64,
}

#[derive(Clone, Copy)]
struct ReplacedTarget {
    private_id: u64,
    has_stream: bool,
}

#[derive(Clone, Copy)]
struct KeptStream {
    refcnt: u32,
    owner: u64,
    size: u64,
}

pub fn build(
    cryptex: &SourceCryptex,
    resolved: &BTreeMap<u64, ResolvedReplacement>,
    time_ns: u64,
) -> Result<BuiltRecords, SealError> {
    let mut kept_data: BTreeMap<u64, KeptStream> = BTreeMap::new();
    let mut kept_xattr_streams: BTreeMap<u64, (u64, u64)> = BTreeMap::new();
    let mut dropped_security: HashSet<u64> = HashSet::new();
    let mut dropped_rsrc_fork: HashSet<u64> = HashSet::new();

    for (&oid, inode) in &cryptex.inodes {
        if resolved.contains_key(&oid) {
            continue;
        }
        if let Some(d) = inode.dstream()
            && (d.size.get() > 0 || cryptex.fext.contains_key(&inode.val.private_id.get()))
        {
            let sid = inode.val.private_id.get();
            let entry = kept_data.entry(sid).or_insert(KeptStream {
                refcnt: 0,
                owner: oid,
                size: 0,
            });
            entry.refcnt = entry
                .refcnt
                .checked_add(1)
                .ok_or_else(|| SealError::InvalidSource(format!("stream {sid}: refcount overflow")))?;
            entry.owner = entry.owner.min(oid);
            entry.size = entry.size.max(d.size.get());
        }
        if let Some(xs) = cryptex.xattrs.get(&oid) {
            for x in xs {
                match &x.data {
                    XattrData::Embedded(_) => {
                        if !keep_embedded_xattr(&x.name) {
                            if x.name == SECURITY_XATTR_NAME {
                                dropped_security.insert(oid);
                            } else if x.name == RESOURCE_FORK_XATTR_NAME {
                                dropped_rsrc_fork.insert(oid);
                            }
                        }
                    }
                    XattrData::Stream { id, dstream } => {
                        let owner = if x.name == RESOURCE_FORK_XATTR_NAME || x.name == DECMPFS_XATTR_NAME {
                            oid
                        } else {
                            *id
                        };
                        let size = dstream.dstream.size.get();
                        if size > 0 || cryptex.fext.contains_key(id) {
                            kept_xattr_streams
                                .entry(*id)
                                .and_modify(|(cur_owner, cur_size)| {
                                    *cur_owner = (*cur_owner).min(owner);
                                    *cur_size = (*cur_size).max(size);
                                })
                                .or_insert((owner, size));
                        }
                    }
                }
            }
        }
    }

    let mut used_stream_ids: HashSet<u64> = HashSet::new();
    used_stream_ids.extend(kept_data.keys().copied());
    used_stream_ids.extend(kept_xattr_streams.keys().copied());

    let mut next_obj_id = cryptex
        .max_fs_oid
        .checked_add(1)
        .ok_or_else(|| SealError::InvalidSource("source max_fs_oid overflow".into()))?
        .max(cryptex.sb.next_obj_id.get())
        .max(MIN_USER_INO_NUM);

    let mut streams = Vec::new();
    let mut kept_nonempty_source_streams: HashSet<u64> = HashSet::new();
    let mut kept_stream_alloced: HashMap<u64, u64> = HashMap::new();

    for (sid, owner, size, label) in kept_data
        .iter()
        .map(|(&sid, k)| (sid, k.owner, k.size, "data"))
        .chain(kept_xattr_streams.iter().map(|(&sid, &(o, s))| (sid, o, s, "xattr")))
    {
        match cryptex.fext.get(&sid) {
            Some(extents) if !extents.is_empty() => {
                let (blocks, bytes) = extent_bytes(sid, extents)?;
                if bytes < size {
                    return Err(SealError::InvalidSource(format!(
                        "kept {label} stream {sid}: extents cover {bytes} bytes, less than {size}"
                    )));
                }
                kept_nonempty_source_streams.insert(sid);
                kept_stream_alloced.insert(sid, bytes);
                streams.push(PlannedStream {
                    stream_id: sid,
                    owner_oid: owner,
                    blocks,
                    source: StreamSource::Source(extents.clone()),
                });
            }
            _ if size == 0 => {}
            _ => {
                return Err(SealError::InvalidSource(format!(
                    "kept {label} stream {sid} ({size} bytes) has no fext extents"
                )));
            }
        }
    }

    let mut replaced_targets: HashMap<u64, ReplacedTarget> = HashMap::new();
    for (&oid, rep) in resolved {
        // Allocate a fresh private_id whenever `oid` still names a live kept stream, even for a 0-byte replacement.
        let private_id = if used_stream_ids.insert(oid) {
            oid
        } else {
            let id = next_obj_id;
            next_obj_id = next_obj_id
                .checked_add(1)
                .ok_or_else(|| SealError::InvalidSpec("next_obj_id overflow".into()))?;
            used_stream_ids.insert(id);
            id
        };
        let has_stream = rep.hashed.size > 0;
        replaced_targets.insert(oid, ReplacedTarget { private_id, has_stream });
        if has_stream {
            streams.push(PlannedStream {
                stream_id: private_id,
                owner_oid: oid,
                blocks: rep.hashed.size.div_ceil(BLOCK_SIZE as u64),
                source: StreamSource::Replacement {
                    data: rep.data.clone(),
                    size: rep.hashed.size,
                    sha256: rep.hashed.sha256,
                },
            });
        }
    }

    let mut fs = Vec::with_capacity(cryptex.raw_records.len());
    let mut seen_file_info: HashSet<u64> = HashSet::new();
    let (mut files, mut dirs, mut symlinks, mut other) = (0u64, 0u64, 0u64, 0u64);

    for (key, value) in &cryptex.raw_records {
        let (oid, kind) = catalog::key_header(key)?;
        match kind {
            APFS_TYPE_INODE => {
                let inode = cryptex
                    .inodes
                    .get(&oid)
                    .ok_or_else(|| SealError::InvalidSource(format!("missing decoded inode {oid}")))?;
                if oid != ROOT_DIR_INO_NUM && oid != PRIV_DIR_INO_NUM {
                    match inode.val.mode.get() & S_IFMT {
                        S_IFREG => files += 1,
                        S_IFDIR => dirs += 1,
                        S_IFLNK => symlinks += 1,
                        _ => other += 1,
                    }
                }
                let encoded = if let Some(rep) = resolved.get(&oid) {
                    let target = replaced_targets[&oid];
                    encode_replaced_inode(inode, target.private_id, target.has_stream, rep.hashed.size, time_ns)?
                } else {
                    let alloced = kept_stream_alloced.get(&inode.val.private_id.get()).copied();
                    encode_kept_inode(
                        inode,
                        time_ns,
                        dropped_security.contains(&oid),
                        dropped_rsrc_fork.contains(&oid),
                        alloced,
                    )
                };
                fs.push((key.clone(), Some(encoded)));
            }
            APFS_TYPE_DIR_REC => {
                if value.len() < 16 {
                    return Err(SealError::InvalidSource(format!("short drec value under {oid}")));
                }
                let mut val = value.clone();
                val[8..16].copy_from_slice(&time_ns.to_le_bytes());
                fs.push((key.clone(), Some(val)));
            }
            APFS_TYPE_XATTR => {
                if resolved.contains_key(&oid) {
                    continue;
                }
                let x = Xattr::decode(key, value)?;
                if let Some(encoded) = encode_kept_xattr(&x, kept_stream_alloced.get(&xattr_stream_id(&x)).copied()) {
                    fs.push((key.clone(), Some(encoded)));
                }
            }
            APFS_TYPE_SIBLING_LINK | APFS_TYPE_SIBLING_MAP => {
                fs.push((key.clone(), Some(value.clone())));
            }
            APFS_TYPE_DSTREAM_ID => {}
            APFS_TYPE_FILE_INFO => {
                if kept_nonempty_source_streams.contains(&oid) {
                    seen_file_info.insert(oid);
                    fs.push((key.clone(), Some(value.clone())));
                }
            }
            _ => {
                return Err(SealError::InvalidSource(format!(
                    "unexpected record type {kind} for oid {oid}"
                )));
            }
        }
    }

    for &sid in &kept_nonempty_source_streams {
        if !seen_file_info.contains(&sid) {
            return Err(SealError::InvalidSource(format!(
                "source stream {sid} has extents but no FILE_INFO hashes"
            )));
        }
    }

    for (&sid, k) in &kept_data {
        if kept_nonempty_source_streams.contains(&sid) {
            fs.push(encode_dstream_id(sid, k.refcnt));
        }
    }
    for (&oid, rep) in resolved {
        let target = replaced_targets[&oid];
        if target.has_stream {
            fs.push(encode_dstream_id(target.private_id, 1));
            for chunk in &rep.hashed.chunks {
                fs.push(file_info::encode_file_info(target.private_id, chunk));
            }
        }
    }

    sort_fs_records(&mut fs);
    for w in fs.windows(2) {
        if fs_key_order(&w[0].0, &w[1].0) == Ordering::Equal {
            return Err(SealError::InvalidSource("duplicate key in sealed fs tree".into()));
        }
    }

    Ok(BuiltRecords {
        fs,
        streams,
        next_obj_id,
        files,
        dirs,
        symlinks,
        other,
    })
}

pub fn keep_embedded_xattr(name: &str) -> bool {
    name == SYMLINK_XATTR_NAME || name == DECMPFS_XATTR_NAME
}

fn xattr_stream_id(x: &Xattr) -> u64 {
    match x.data {
        XattrData::Stream { id, .. } => id,
        XattrData::Embedded(_) => 0,
    }
}

fn encode_kept_xattr(x: &Xattr, alloced: Option<u64>) -> Option<Vec<u8>> {
    match &x.data {
        XattrData::Embedded(bytes) => {
            if !keep_embedded_xattr(&x.name) {
                return None;
            }
            let head = XattrVal {
                flags: U16::new(x.flags),
                xdata_len: U16::new(bytes.len() as u16),
            };
            let mut val = head.as_bytes().to_vec();
            val.extend_from_slice(bytes);
            Some(val)
        }
        XattrData::Stream { id, dstream } => {
            let mut d = *dstream;
            if let Some(alloced) = alloced {
                d.dstream.alloced_size = U64::new(alloced);
            }
            d.dstream.total_bytes_written = U64::new(1);
            d.dstream.total_bytes_read = U64::new(1);
            d.xattr_obj_id = U64::new(*id);
            let flags = (x.flags & !XATTR_SETXATTR_STREAM) | XATTR_DATA_STREAM;
            let head = XattrVal {
                flags: U16::new(flags),
                xdata_len: U16::new(size_of::<XattrDstream>() as u16),
            };
            let mut val = head.as_bytes().to_vec();
            val.extend_from_slice(d.as_bytes());
            Some(val)
        }
    }
}

fn encode_kept_inode(
    inode: &Inode,
    time_ns: u64,
    dropped_security: bool,
    dropped_rsrc_fork: bool,
    alloced: Option<u64>,
) -> Vec<u8> {
    let mut val = inode.val;
    let t = U64::new(time_ns);
    val.create_time = t;
    val.mod_time = t;
    val.change_time = t;
    val.access_time = t;
    val.write_generation_counter = U32::new(1);
    let mut iflags = val.internal_flags.get();
    if dropped_security {
        iflags &= !INODE_HAS_SECURITY_EA;
    }
    if dropped_rsrc_fork {
        iflags = (iflags & !INODE_HAS_RSRC_FORK) | INODE_NO_RSRC_FORK;
    }
    val.internal_flags = U64::new(iflags);
    let fields: Vec<XField> = inode
        .xfields
        .iter()
        .filter_map(|xf| {
            if xf.kind == INO_EXT_TYPE_DSTREAM {
                let alloced = alloced?;
                let mut d = inode.dstream()?;
                d.alloced_size = U64::new(alloced);
                d.total_bytes_written = U64::new(1);
                d.total_bytes_read = U64::new(1);
                return Some(XField {
                    kind: xf.kind,
                    flags: xf.flags,
                    data: d.as_bytes().to_vec(),
                });
            }
            Some(xf.clone())
        })
        .collect();
    let mut out = val.as_bytes().to_vec();
    out.extend_from_slice(&xfields(&fields));
    out
}

fn encode_replaced_inode(
    inode: &Inode,
    private_id: u64,
    has_stream: bool,
    size: u64,
    time_ns: u64,
) -> Result<Vec<u8>, SealError> {
    let mut val = inode.val;
    let t = U64::new(time_ns);
    val.private_id = U64::new(private_id);
    val.create_time = t;
    val.mod_time = t;
    val.change_time = t;
    val.access_time = t;
    val.internal_flags =
        U64::new((replaced_flags(val.internal_flags.get()) & !INODE_HAS_SECURITY_EA) | INODE_NO_RSRC_FORK);
    val.write_generation_counter = U32::new(1);
    val.bsd_flags = U32::new(val.bsd_flags.get() & !UF_COMPRESSED);
    val.uncompressed_size = U64::ZERO;

    let mut fields: Vec<XField> = inode
        .xfields
        .iter()
        .filter(|xf| xf.kind != INO_EXT_TYPE_DSTREAM && xf.kind != INO_EXT_TYPE_SPARSE_BYTES)
        .cloned()
        .collect();
    if has_stream {
        let alloced = size
            .checked_next_multiple_of(BLOCK_SIZE as u64)
            .ok_or_else(|| SealError::InvalidSpec("replacement file size overflow".into()))?;
        let d = Dstream {
            size: U64::new(size),
            alloced_size: U64::new(alloced),
            default_crypto_id: U64::ZERO,
            total_bytes_written: U64::new(1),
            total_bytes_read: U64::new(1),
        };
        fields.push(XField {
            kind: INO_EXT_TYPE_DSTREAM,
            flags: XF_SYSTEM_FIELD,
            data: d.as_bytes().to_vec(),
        });
    }
    let mut out = val.as_bytes().to_vec();
    out.extend_from_slice(&xfields(&fields));
    Ok(out)
}

fn encode_dstream_id(stream_id: u64, refcnt: u32) -> Record {
    (
        JKey::new(stream_id, APFS_TYPE_DSTREAM_ID).as_bytes().to_vec(),
        Some(
            DstreamIdVal {
                refcnt: U32::new(refcnt),
            }
            .as_bytes()
            .to_vec(),
        ),
    )
}
