// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
// Forked from dpp's apfs 0.4.1 src/catalog.rs (InodeVal, DrecVal, FileExtentRecord, the xfield walk, the xattr value
// and the catalog key order), MIT, Copyright (c) 2026 Dil4rd; see LICENSE-dpp.
// Changed: zerocopy layouts; every xfield kept with its flags; each record's raw key and value kept beside its
// decoded view; the whole j_dstream of a stream xattr; siblings, file info, physical extents and directory stats;
// lengths checked exactly, and names refused unless they are NUL-terminated UTF-8.

use std::cmp::Ordering;

use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_EXTENT, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE, APFS_TYPE_XATTR,
    DrecVal, Dstream, DstreamIdVal, FileExtentKey, FileExtentVal, INO_EXT_TYPE_DSTREAM, INO_EXT_TYPE_NAME, InodeVal,
    J_DREC_HASH_SHIFT, J_DREC_LEN_MASK, J_FILE_EXTENT_LEN_MASK, JKey, PEXT_KIND_SHIFT, PEXT_LEN_MASK, PhysExtVal,
    S_IFDIR, S_IFREG,
};
use switch_apfs_format::fs_records::{
    APFS_TYPE_DIR_STATS, APFS_TYPE_FILE_INFO, APFS_TYPE_SIBLING_LINK, APFS_TYPE_SIBLING_MAP, DREC_EXT_TYPE_SIBLING_ID,
    DREC_TYPE_MASK, DirStatsVal, FextKey, FextVal, FileDataHashVal, FileInfoKey, J_FILE_INFO_LBA_MASK,
    J_FILE_INFO_TYPE_SHIFT, S_IFLNK, S_IFMT, SiblingKey, SiblingVal, UF_COMPRESSED, XATTR_DATA_STREAM, XattrDstream,
    XattrVal,
};
use switch_apfs_format::get;
use zerocopy::FromBytes;

use crate::ReadError;

/// The `(object id, record type)` every fs-tree key starts with.
pub fn key_header(key: &[u8]) -> Result<(u64, u8), ReadError> {
    let k: JKey = get(key, 0).ok_or_else(|| ReadError::invalid("fs-tree key shorter than its header"))?;
    Ok((k.oid(), k.kind()))
}

/// The fs tree sorts by object id, then by record type: not by the packed little-endian word they share, whose type
/// is in the high nibble.
pub fn order(key: &[u8], oid: u64, kind: Option<u8>) -> Result<Ordering, ReadError> {
    let (key_oid, key_kind) = key_header(key)?;
    Ok(match (key_oid.cmp(&oid), kind) {
        (Ordering::Equal, Some(kind)) => key_kind.cmp(&kind),
        (ord, _) => ord,
    })
}

fn exact<T: FromBytes>(bytes: &[u8], what: &str) -> Result<T, ReadError> {
    if bytes.len() != size_of::<T>() {
        return Err(ReadError::invalid(format!(
            "{what}: {} bytes, {} expected",
            bytes.len(),
            size_of::<T>()
        )));
    }
    get(bytes, 0).ok_or_else(|| ReadError::invalid(what))
}

fn prefix<T: FromBytes>(bytes: &[u8], what: &str) -> Result<T, ReadError> {
    get(bytes, 0).ok_or_else(|| ReadError::invalid(format!("{what}: {} bytes is too short", bytes.len())))
}

/// A NUL-terminated name of exactly `len` bytes, terminator included.
fn name(bytes: &[u8], what: &str) -> Result<String, ReadError> {
    let Some((&0, text)) = bytes.split_last() else {
        return Err(ReadError::invalid(format!("{what}: name isn't NUL-terminated")));
    };
    if text.contains(&0) {
        return Err(ReadError::invalid(format!("{what}: NUL inside the name")));
    }
    String::from_utf8(text.to_vec()).map_err(|_| ReadError::invalid(format!("{what}: name isn't UTF-8")))
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct XField {
    pub kind: u8,
    pub flags: u8,
    pub data: Vec<u8>,
}

/// An `xf_blob_t`: the count and used size, every field's header, then every field's data, each padded to 8 bytes.
/// The data starts right after the headers, unaligned.
pub fn xfields(blob: &[u8]) -> Result<Vec<XField>, ReadError> {
    if blob.is_empty() {
        return Ok(Vec::new());
    }
    let count = usize::from(u16::from_le_bytes(prefix(blob, "xfields")?));
    let used = usize::from(u16::from_le_bytes(prefix(&blob[2..], "xfields")?));
    let mut at = 4 + 4 * count;
    if at + used != blob.len() {
        return Err(ReadError::invalid(format!(
            "xfields: {count} fields and {used} data bytes in {} bytes",
            blob.len()
        )));
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let h = &blob[4 + 4 * i..8 + 4 * i];
        let size = usize::from(u16::from_le_bytes([h[2], h[3]]));
        let data = blob
            .get(at..at + size)
            .ok_or_else(|| ReadError::invalid(format!("xfield {i} past its blob")))?;
        out.push(XField {
            kind: h[0],
            flags: h[1],
            data: data.to_vec(),
        });
        at += size.next_multiple_of(8);
    }
    if at != blob.len() {
        return Err(ReadError::invalid("xfields: padded data doesn't fill the used size"));
    }
    Ok(out)
}

#[derive(Clone, Debug)]
pub struct Inode {
    pub oid: u64,
    pub val: InodeVal,
    pub xfields: Vec<XField>,
    pub value: Vec<u8>,
}

impl Inode {
    pub fn decode(oid: u64, value: &[u8]) -> Result<Self, ReadError> {
        let val: InodeVal = prefix(value, "inode")?;
        Ok(Self {
            oid,
            val,
            xfields: xfields(&value[size_of::<InodeVal>()..])
                .map_err(|e| ReadError::invalid(format!("inode {oid}: {e}")))?,
            value: value.to_vec(),
        })
    }

    pub fn kind(&self) -> u16 {
        self.val.mode.get() & S_IFMT
    }

    pub fn is_dir(&self) -> bool {
        self.kind() == S_IFDIR
    }

    pub fn is_regular(&self) -> bool {
        self.kind() == S_IFREG
    }

    pub fn is_symlink(&self) -> bool {
        self.kind() == S_IFLNK
    }

    pub fn is_compressed(&self) -> bool {
        self.val.bsd_flags.get() & UF_COMPRESSED != 0
    }

    pub fn xfield(&self, kind: u8) -> Option<&XField> {
        self.xfields.iter().find(|x| x.kind == kind)
    }

    pub fn name(&self) -> Option<String> {
        self.xfield(INO_EXT_TYPE_NAME)
            .and_then(|x| name(&x.data, "inode name").ok())
    }

    pub fn dstream(&self) -> Option<Dstream> {
        self.xfield(INO_EXT_TYPE_DSTREAM)
            .and_then(|x| exact(&x.data, "dstream").ok())
    }
}

#[derive(Clone, Debug)]
pub struct DirRec {
    pub parent: u64,
    pub name: String,
    /// The 22-bit name hash, on a volume whose directory keys are hashed.
    pub hash: Option<u32>,
    pub val: DrecVal,
    pub xfields: Vec<XField>,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}

impl DirRec {
    pub fn decode(key: &[u8], value: &[u8], hashed: bool) -> Result<Self, ReadError> {
        let (parent, _) = key_header(key)?;
        let (hash, name_at, len) = if hashed {
            let word = u32::from_le_bytes(prefix(&key[8..], "drec key")?);
            (Some(word >> J_DREC_HASH_SHIFT), 12, (word & J_DREC_LEN_MASK) as usize)
        } else {
            (
                None,
                10,
                usize::from(u16::from_le_bytes(prefix(&key[8..], "drec key")?)),
            )
        };
        if key.len() != name_at + len {
            return Err(ReadError::invalid(format!(
                "drec under {parent}: name length {len} in a {}-byte key",
                key.len()
            )));
        }
        let val: DrecVal = prefix(value, "drec value")?;
        Ok(Self {
            parent,
            name: name(&key[name_at..], "drec")?,
            hash,
            val,
            xfields: xfields(&value[size_of::<DrecVal>()..])?,
            key: key.to_vec(),
            value: value.to_vec(),
        })
    }

    pub fn file_id(&self) -> u64 {
        self.val.file_id.get()
    }

    pub fn dtype(&self) -> u16 {
        self.val.flags.get() & DREC_TYPE_MASK
    }

    pub fn sibling_id(&self) -> Option<u64> {
        let x = self.xfields.iter().find(|x| x.kind == DREC_EXT_TYPE_SIBLING_ID)?;
        exact::<[u8; 8]>(&x.data, "sibling id").ok().map(u64::from_le_bytes)
    }
}

#[derive(Clone, Debug)]
pub enum XattrData {
    Embedded(Vec<u8>),
    /// Stored like file data, in the stream `id`'s extents.
    Stream {
        id: u64,
        dstream: XattrDstream,
    },
}

#[derive(Clone, Debug)]
pub struct Xattr {
    pub oid: u64,
    pub name: String,
    pub flags: u16,
    pub data: XattrData,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}

impl Xattr {
    pub fn decode(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let (oid, _) = key_header(key)?;
        let len = usize::from(u16::from_le_bytes(prefix(&key[8..], "xattr key")?));
        if key.len() != 10 + len {
            return Err(ReadError::invalid(format!(
                "xattr of {oid}: name length {len} in a {}-byte key",
                key.len()
            )));
        }
        let val: XattrVal = prefix(value, "xattr value")?;
        let xdata = &value[size_of::<XattrVal>()..];
        if xdata.len() != usize::from(val.xdata_len.get()) {
            return Err(ReadError::invalid(format!(
                "xattr of {oid}: xdata_len disagrees with the value"
            )));
        }
        let data = if val.flags.get() & XATTR_DATA_STREAM != 0 {
            let dstream: XattrDstream = exact(xdata, "xattr stream")?;
            XattrData::Stream {
                id: dstream.xattr_obj_id.get(),
                dstream,
            }
        } else {
            XattrData::Embedded(xdata.to_vec())
        };
        Ok(Self {
            oid,
            name: name(&key[10..], "xattr")?,
            flags: val.flags.get(),
            data,
            key: key.to_vec(),
            value: value.to_vec(),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SiblingLink {
    pub oid: u64,
    pub sibling_id: u64,
    pub parent_id: u64,
    pub name: String,
}

impl SiblingLink {
    pub fn decode(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let k: SiblingKey = exact(key, "sibling key")?;
        let v: SiblingVal = prefix(value, "sibling value")?;
        let text = &value[size_of::<SiblingVal>()..];
        if text.len() != usize::from(v.name_len.get()) {
            return Err(ReadError::invalid("sibling name length"));
        }
        Ok(Self {
            oid: k.hdr.oid(),
            sibling_id: k.sibling_id.get(),
            parent_id: v.parent_id.get(),
            name: name(text, "sibling")?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileExtent {
    pub stream: u64,
    pub logical_addr: u64,
    pub len: u64,
    pub flags: u8,
    /// 0 for a hole.
    pub phys_block_num: u64,
    /// Not in the fext tree; 0 there.
    pub crypto_id: u64,
}

impl FileExtent {
    pub fn decode(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let k: FileExtentKey = exact(key, "file-extent key")?;
        let v: FileExtentVal = exact(value, "file-extent value")?;
        Ok(Self {
            stream: k.hdr.oid(),
            logical_addr: k.logical_addr.get(),
            len: v.len_and_flags.get() & J_FILE_EXTENT_LEN_MASK,
            flags: (v.len_and_flags.get() >> 56) as u8,
            phys_block_num: v.phys_block_num.get(),
            crypto_id: v.crypto_id.get(),
        })
    }

    pub fn decode_fext(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let k: FextKey = exact(key, "fext key")?;
        let v: FextVal = exact(value, "fext value")?;
        Ok(Self {
            stream: k.private_id.get(),
            logical_addr: k.logical_addr.get(),
            len: v.len_and_flags.get() & J_FILE_EXTENT_LEN_MASK,
            flags: (v.len_and_flags.get() >> 56) as u8,
            phys_block_num: v.phys_block_num.get(),
            crypto_id: 0,
        })
    }

    pub fn blocks(&self) -> u64 {
        self.len.div_ceil(switch_apfs_format::BLOCK_SIZE as u64)
    }
}

/// A sealed volume's hash of one stretch of a data stream.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileInfo {
    pub stream: u64,
    pub info_type: u8,
    /// In bytes, though the Reference calls it an LBA: 0, 16384, 32768, … in Apple's own cryptexes.
    pub offset: u64,
    /// In blocks.
    pub hashed_len: u16,
    pub hash: Vec<u8>,
}

impl FileInfo {
    pub fn decode(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let k: FileInfoKey = exact(key, "file-info key")?;
        let v: FileDataHashVal = prefix(value, "file-info value")?;
        let hash = &value[size_of::<FileDataHashVal>()..];
        if hash.len() != usize::from(v.hash_size) {
            return Err(ReadError::invalid("file-info hash size"));
        }
        Ok(Self {
            stream: k.hdr.oid(),
            info_type: (k.info_and_lba.get() >> J_FILE_INFO_TYPE_SHIFT) as u8,
            offset: k.info_and_lba.get() & J_FILE_INFO_LBA_MASK,
            hashed_len: v.hashed_len.get(),
            hash: hash.to_vec(),
        })
    }
}

/// An extent-reference record: who owns a run of blocks, and how many references it has.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PhysExtent {
    pub paddr: u64,
    pub blocks: u64,
    pub kind: u8,
    pub owner: u64,
    pub refcnt: i32,
}

impl PhysExtent {
    pub fn decode(key: &[u8], value: &[u8]) -> Result<Self, ReadError> {
        let (paddr, kind) = key_header(key)?;
        if key.len() != size_of::<JKey>() || kind != APFS_TYPE_EXTENT {
            return Err(ReadError::invalid("extent-reference key"));
        }
        let v: PhysExtVal = exact(value, "extent-reference value")?;
        Ok(Self {
            paddr,
            blocks: v.len_and_kind.get() & PEXT_LEN_MASK,
            kind: (v.len_and_kind.get() >> PEXT_KIND_SHIFT) as u8,
            owner: v.owning_obj_id.get(),
            refcnt: v.refcnt.get(),
        })
    }
}

#[derive(Clone, Debug)]
pub enum Record {
    Inode(Inode),
    DirRec(DirRec),
    Xattr(Xattr),
    SiblingLink(SiblingLink),
    SiblingMap {
        oid: u64,
        file_id: u64,
    },
    DstreamId {
        oid: u64,
        refcnt: u32,
    },
    FileExtent(FileExtent),
    FileInfo(FileInfo),
    DirStats {
        oid: u64,
        val: DirStatsVal,
    },
    /// Snapshot, crypto-state and any other record kept as stored.
    Other {
        oid: u64,
        kind: u8,
    },
}

impl Record {
    pub fn decode(key: &[u8], value: &[u8], hashed_names: bool) -> Result<Self, ReadError> {
        let (oid, kind) = key_header(key)?;
        let exact_key = |what: &str| {
            if key.len() == size_of::<JKey>() {
                Ok(())
            } else {
                Err(ReadError::invalid(format!("{what} key of {oid}: {} bytes", key.len())))
            }
        };
        Ok(match kind {
            APFS_TYPE_INODE => {
                exact_key("inode")?;
                Record::Inode(Inode::decode(oid, value)?)
            }
            APFS_TYPE_DIR_REC => Record::DirRec(DirRec::decode(key, value, hashed_names)?),
            APFS_TYPE_XATTR => Record::Xattr(Xattr::decode(key, value)?),
            APFS_TYPE_SIBLING_LINK => Record::SiblingLink(SiblingLink::decode(key, value)?),
            APFS_TYPE_SIBLING_MAP => {
                exact_key("sibling map")?;
                Record::SiblingMap {
                    oid,
                    file_id: u64::from_le_bytes(exact(value, "sibling map")?),
                }
            }
            APFS_TYPE_DSTREAM_ID => {
                exact_key("dstream id")?;
                Record::DstreamId {
                    oid,
                    refcnt: exact::<DstreamIdVal>(value, "dstream id")?.refcnt.get(),
                }
            }
            APFS_TYPE_FILE_EXTENT => Record::FileExtent(FileExtent::decode(key, value)?),
            APFS_TYPE_FILE_INFO => Record::FileInfo(FileInfo::decode(key, value)?),
            APFS_TYPE_DIR_STATS => {
                exact_key("dir stats")?;
                Record::DirStats {
                    oid,
                    val: exact(value, "dir stats")?,
                }
            }
            _ => Record::Other { oid, kind },
        })
    }
}
