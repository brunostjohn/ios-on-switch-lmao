// SPDX-License-Identifier: GPL-3.0-or-later
//! A new volume's file-system records, laid out as macOS 27.2's kext writes them for the same files.

use std::cmp::Ordering;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::{
    APFS_KIND_NEW, APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_EXTENT, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE,
    DT_DIR, DT_REG, DrecVal, Dstream, DstreamIdVal, FileExtentKey, FileExtentVal, INO_EXT_TYPE_DSTREAM,
    INO_EXT_TYPE_NAME, INODE_NO_RSRC_FORK, InodeVal, JKey, MIN_USER_INO_NUM, PEXT_KIND_SHIFT, PRIV_DIR_INO_NUM,
    PRIV_DIR_NAME, PhysExtVal, ROOT_DIR_INO_NUM, ROOT_DIR_NAME, ROOT_DIR_PARENT, S_IFDIR, S_IFREG, XF_DO_NOT_COPY,
    XF_SYSTEM_FIELD, XField, XfBlob,
};
use switch_apfs_format::volume::APFS_INCOMPAT_EXPANDED_RECORDS;
use switch_apfs_names::{UnicodeVersion, name_len_and_hash};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{I32, U16, U32, U64};

use crate::WriteError;
use crate::spec::{Content, Entry, Owner, Tree};

pub struct FileExtent {
    pub inode: u64,
    pub blocks: u64,
}

/// The tree with inode numbers assigned and file data waiting for addresses.
pub struct Numbered<'a> {
    pub nodes: Vec<Placed<'a>>,
    pub next_inode: u64,
    pub files: u64,
    pub dirs: u64,
}

pub struct Placed<'a> {
    pub inode: u64,
    pub parent: u64,
    pub entry: &'a Entry,
}

pub fn number(tree: &Tree) -> Numbered<'_> {
    let mut out = Numbered {
        nodes: Vec::new(),
        next_inode: MIN_USER_INO_NUM,
        files: 0,
        dirs: 0,
    };
    place(&tree.entries, ROOT_DIR_INO_NUM, &mut out);
    out
}

fn place<'a>(entries: &'a [Entry], parent: u64, out: &mut Numbered<'a>) {
    for entry in entries {
        let inode = out.next_inode;
        out.next_inode += 1;
        out.nodes.push(Placed { inode, parent, entry });
        match &entry.content {
            Content::Dir(children) => {
                out.dirs += 1;
                place(children, inode, out);
            }
            Content::File(_) => out.files += 1,
        }
    }
}

impl Numbered<'_> {
    pub fn extents(&self) -> Vec<FileExtent> {
        self.nodes
            .iter()
            .filter_map(|n| match &n.entry.content {
                Content::File(data) if !data.is_empty() => Some(FileExtent {
                    inode: n.inode,
                    blocks: (data.len() as u64).div_ceil(BLOCK_SIZE as u64),
                }),
                _ => None,
            })
            .collect()
    }
}

pub struct RecordInput<'a> {
    pub tree: &'a Tree,
    pub numbered: &'a Numbered<'a>,
    /// The first block of each file's single extent, in `Numbered::extents` order.
    pub extent_addrs: &'a [u64],
    pub incompatible_features: u64,
    pub unicode: UnicodeVersion,
    pub time_ns: u64,
}

pub type Record = (Vec<u8>, Option<Vec<u8>>);

/// The fs-tree records and the extent-reference records, each in key order.
pub fn build(input: &RecordInput) -> Result<(Vec<Record>, Vec<Record>), WriteError> {
    let mut fs = Vec::new();
    let mut extent_refs = Vec::new();
    let subdirs = |children: &[Entry]| children.iter().filter(|e| matches!(e.content, Content::Dir(_))).count();
    let dir_size = |children: &[Entry]| {
        if input.incompatible_features & APFS_INCOMPAT_EXPANDED_RECORDS != 0 {
            subdirs(children) as u64
        } else {
            0
        }
    };

    let root = &input.tree.root;
    fs.push(drec(input, ROOT_DIR_PARENT, ROOT_DIR_NAME, ROOT_DIR_INO_NUM, DT_DIR)?);
    fs.push(drec(input, ROOT_DIR_PARENT, PRIV_DIR_NAME, PRIV_DIR_INO_NUM, DT_DIR)?);
    let children = &input.tree.entries;
    fs.push(inode(
        input,
        ROOT_DIR_PARENT,
        ROOT_DIR_INO_NUM,
        root,
        S_IFDIR,
        children.len(),
        dir_size(children),
        ROOT_DIR_NAME,
        None,
    ));
    let private = Owner {
        uid: 0,
        gid: 0,
        mode: 0o644,
    };
    fs.push(inode(
        input,
        ROOT_DIR_PARENT,
        PRIV_DIR_INO_NUM,
        &private,
        S_IFDIR,
        0,
        0,
        PRIV_DIR_NAME,
        None,
    ));

    let extents = input.numbered.extents();
    for placed in &input.numbered.nodes {
        let entry = placed.entry;
        match &entry.content {
            Content::Dir(children) => {
                fs.push(drec(input, placed.parent, &entry.name, placed.inode, DT_DIR)?);
                let size = dir_size(children);
                fs.push(inode(
                    input,
                    placed.parent,
                    placed.inode,
                    &entry.owner,
                    S_IFDIR,
                    children.len(),
                    size,
                    &entry.name,
                    None,
                ));
            }
            Content::File(data) => {
                fs.push(drec(input, placed.parent, &entry.name, placed.inode, DT_REG)?);
                let extent = extents.iter().position(|e| e.inode == placed.inode);
                let stream = extent.map(|n| (data.len() as u64, extents[n].blocks));
                fs.push(inode(
                    input,
                    placed.parent,
                    placed.inode,
                    &entry.owner,
                    S_IFREG,
                    1,
                    0,
                    &entry.name,
                    stream,
                ));
                if let Some(n) = extent {
                    let paddr = input.extent_addrs[n];
                    fs.push((
                        JKey::new(placed.inode, APFS_TYPE_DSTREAM_ID).as_bytes().to_vec(),
                        Some(DstreamIdVal { refcnt: U32::new(1) }.as_bytes().to_vec()),
                    ));
                    let key = FileExtentKey {
                        hdr: JKey::new(placed.inode, APFS_TYPE_FILE_EXTENT),
                        logical_addr: U64::ZERO,
                    };
                    let value = FileExtentVal {
                        len_and_flags: U64::new(extents[n].blocks * BLOCK_SIZE as u64),
                        phys_block_num: U64::new(paddr),
                        crypto_id: U64::ZERO,
                    };
                    fs.push((key.as_bytes().to_vec(), Some(value.as_bytes().to_vec())));
                    let value = PhysExtVal {
                        len_and_kind: U64::new(extents[n].blocks | APFS_KIND_NEW << PEXT_KIND_SHIFT),
                        owning_obj_id: U64::new(placed.inode),
                        refcnt: I32::new(1),
                    };
                    extent_refs.push((
                        JKey::new(paddr, APFS_TYPE_EXTENT).as_bytes().to_vec(),
                        Some(value.as_bytes().to_vec()),
                    ));
                }
            }
        }
    }
    fs.sort_by(|a, b| fs_key_order(&a.0, &b.0));
    extent_refs.sort_by(|a, b| fs_key_order(&a.0, &b.0));
    Ok((fs, extent_refs))
}

/// File-system keys sort by object id, then record type, then the type's own key: a directory entry's hash and
/// name, a file extent's logical address.
pub fn fs_key_order(a: &[u8], b: &[u8]) -> Ordering {
    let head = |k: &[u8]| {
        let v = u64::from_le_bytes(k[..8].try_into().unwrap_or_default());
        (v & switch_apfs_format::fs::OBJ_ID_MASK, v >> 60)
    };
    head(a).cmp(&head(b)).then_with(|| match head(a).1 as u8 {
        APFS_TYPE_DIR_REC => {
            let hash = |k: &[u8]| u32::from_le_bytes(k[8..12].try_into().unwrap_or_default()) >> 10;
            hash(a).cmp(&hash(b)).then_with(|| a[12..].cmp(&b[12..]))
        }
        APFS_TYPE_FILE_EXTENT => u64_at(a, 8).cmp(&u64_at(b, 8)),
        _ => a[8..].cmp(&b[8..]),
    })
}

fn u64_at(k: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(k[at..at + 8].try_into().unwrap_or_default())
}

fn drec(input: &RecordInput, parent: u64, name: &str, inode: u64, kind: u16) -> Result<Record, WriteError> {
    let mut key = JKey::new(parent, APFS_TYPE_DIR_REC).as_bytes().to_vec();
    key.extend_from_slice(&name_len_and_hash(name, input.unicode)?.to_le_bytes());
    key.extend_from_slice(name.as_bytes());
    key.push(0);
    let value = DrecVal {
        file_id: U64::new(inode),
        date_added: U64::new(input.time_ns),
        flags: U16::new(kind),
    };
    Ok((key, Some(value.as_bytes().to_vec())))
}

#[allow(clippy::too_many_arguments)]
fn inode(
    input: &RecordInput,
    parent: u64,
    id: u64,
    owner: &Owner,
    kind: u16,
    nchildren_or_nlink: usize,
    uncompressed_size: u64,
    name: &str,
    stream: Option<(u64, u64)>,
) -> Record {
    let t = U64::new(input.time_ns);
    let value = InodeVal {
        parent_id: U64::new(parent),
        private_id: U64::new(id),
        create_time: t,
        mod_time: t,
        change_time: t,
        access_time: t,
        internal_flags: U64::new(INODE_NO_RSRC_FORK),
        nchildren_or_nlink: I32::new(nchildren_or_nlink as i32),
        default_protection_class: U32::ZERO,
        write_generation_counter: U32::ZERO,
        bsd_flags: U32::ZERO,
        owner: U32::new(owner.uid),
        group: U32::new(owner.gid),
        mode: U16::new(kind | owner.mode),
        pad1: U16::ZERO,
        uncompressed_size: U64::new(uncompressed_size),
    };
    let mut bytes = value.as_bytes().to_vec();
    let mut name_field = name.as_bytes().to_vec();
    name_field.push(0);
    let mut fields = vec![(INO_EXT_TYPE_NAME, XF_DO_NOT_COPY, name_field)];
    if let Some((size, blocks)) = stream {
        let dstream = Dstream {
            size: U64::new(size),
            alloced_size: U64::new(blocks * BLOCK_SIZE as u64),
            default_crypto_id: U64::ZERO,
            total_bytes_written: U64::new(size),
            total_bytes_read: U64::ZERO,
        };
        fields.push((INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD, dstream.as_bytes().to_vec()));
    }
    bytes.extend_from_slice(&xfields(&fields));
    (JKey::new(id, APFS_TYPE_INODE).as_bytes().to_vec(), Some(bytes))
}

/// `xf_blob_t`: the field headers, then each field's data padded to 8 bytes.
fn xfields(fields: &[(u8, u8, Vec<u8>)]) -> Vec<u8> {
    let data: usize = fields.iter().map(|(_, _, d)| d.len().next_multiple_of(8)).sum();
    let blob = XfBlob {
        num_exts: U16::new(fields.len() as u16),
        used_data: U16::new(data as u16),
    };
    let mut out = blob.as_bytes().to_vec();
    for (kind, flags, d) in fields {
        out.extend_from_slice(
            XField {
                kind: *kind,
                flags: *flags,
                size: U16::new(d.len() as u16),
            }
            .as_bytes(),
        );
    }
    for (_, _, d) in fields {
        out.extend_from_slice(d);
        out.extend(std::iter::repeat_n(0, d.len().next_multiple_of(8) - d.len()));
    }
    out
}
