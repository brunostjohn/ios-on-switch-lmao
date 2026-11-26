// SPDX-License-Identifier: GPL-3.0-or-later
//! A volume's fs-tree and extent-reference records, laid out as macOS 27.2's kext writes them for the same files.
//! Inodes keep the numbers the tree gave them; `ids` gives every other object id.

use switch_apfs_format::clone_mapping::{
    APFS_ROCOMPAT_CLONE_MAPPING, APFS_TYPE_CLONE_MAPPING, CLONE_MAPPING_FLAGS, CLONE_MAPPING_OID, CloneMappingKey,
    CloneMappingVal,
};
use switch_apfs_format::fs::{
    APFS_TYPE_DIR_REC, APFS_TYPE_DSTREAM_ID, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE, APFS_TYPE_XATTR, DT_DIR, DT_LNK,
    DT_REG, DrecVal, DstreamIdVal, FileExtentKey, FileExtentVal, JKey, PRIV_DIR_INO_NUM, PRIV_DIR_NAME,
    ROOT_DIR_INO_NUM, ROOT_DIR_NAME, ROOT_DIR_PARENT, XF_DO_NOT_COPY,
};
use switch_apfs_format::fs_records::{
    APFS_TYPE_SIBLING_LINK, APFS_TYPE_SIBLING_MAP, DREC_EXT_TYPE_SIBLING_ID, RESOURCE_FORK_XATTR_NAME, SiblingKey,
    SiblingVal, XATTR_DATA_EMBEDDED, XATTR_DATA_STREAM, XattrDstream, XattrVal,
};
use switch_apfs_format::volume::{APFS_INCOMPAT_CASE_INSENSITIVE, APFS_INCOMPAT_EXPANDED_RECORDS};
use switch_apfs_names::{UnicodeVersion, name_len_and_hash};
use switch_apfs_read::XField;
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U32, U64};

use crate::WriteError;
use crate::extents::{self, Owned};
use crate::ids::{Ids, ids};
use crate::inode::{self, InodeFields};
use crate::keys;
use crate::place::{PlacedExtent, physical_bytes};
use crate::tree::{Attrs, Inode, InodeRef, Kind, Link, Tree, XattrValue};

pub type Record = (Vec<u8>, Option<Vec<u8>>);

const INO_EXT_TYPE_DOCUMENT_ID: u8 = 3;

pub struct RecordInput<'a> {
    pub tree: &'a Tree,
    /// Per stream, from `place`.
    pub extents: &'a [Vec<PlacedExtent>],
    pub incompatible_features: u64,
    pub readonly_compatible_features: u64,
    pub unicode: UnicodeVersion,
    pub time_ns: u64,
}

#[derive(Default)]
pub struct Records {
    pub fs: Vec<Record>,
    pub extent_refs: Vec<Record>,
    pub next_obj_id: u64,
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub data_blocks: u64,
    pub shared_blocks: u64,
    pub clone_mappings: u64,
    /// The highest document id a copied inode carries, or 0.
    pub max_doc_id: u32,
}

pub fn build(input: &RecordInput) -> Result<Records, WriteError> {
    if input.incompatible_features & APFS_INCOMPAT_CASE_INSENSITIVE != 0 {
        return Err(WriteError::Refused(
            "a case-insensitive volume: every name is hashed as a case-sensitive volume hashes it".into(),
        ));
    }
    let tree = input.tree;
    let ids = ids(tree)?;
    let cloned = ids.cloned();
    let count = tree.inode_count();
    let mut children = vec![0u32; count];
    let mut subdirs = vec![0u64; count];
    for r in tree.inode_refs() {
        let node = tree.inode(r);
        for link in &node.links {
            children[link.parent.0 as usize] += 1;
            if node.kind == Kind::Dir {
                subdirs[link.parent.0 as usize] += 1;
            }
        }
    }
    let expanded = input.incompatible_features & APFS_INCOMPAT_EXPANDED_RECORDS != 0;
    let mut out = Records {
        next_obj_id: ids.next,
        ..Default::default()
    };
    let fs = &mut out.fs;
    for (name, number) in [(ROOT_DIR_NAME, ROOT_DIR_INO_NUM), (PRIV_DIR_NAME, PRIV_DIR_INO_NUM)] {
        let link = Named {
            parent: ROOT_DIR_PARENT,
            name,
            hash: None,
            date_added: None,
        };
        fs.push(drec(input, &link, number, DT_DIR, &[])?);
    }
    fs.push(private_dir(input));

    for r in tree.inode_refs() {
        let node = tree.inode(r);
        let i = r.0 as usize;
        let number = tree.number(r);
        let (parent, name) = match node.links.first() {
            Some(l) => (tree.number(l.parent), l.name.as_str()),
            None => (ROOT_DIR_PARENT, ROOT_DIR_NAME),
        };
        let data = node.data.map(|s| (tree.stream(s), &input.extents[s.0 as usize][..]));
        let (nchildren, uncompressed_size) = match node.kind {
            Kind::Dir => (children[i], if expanded { subdirs[i] } else { 0 }),
            _ => (
                node.links.len() as u32,
                node.stored.as_ref().map_or(0, |s| s.uncompressed_size),
            ),
        };
        let value = inode::value(&InodeFields {
            inode: node,
            parent,
            private_id: ids.private[i],
            cloned: cloned[i],
            name,
            nchildren_or_nlink: nchildren,
            uncompressed_size,
            data,
            time_ns: input.time_ns,
        });
        fs.push((key(number, APFS_TYPE_INODE), Some(value)));
        match node.kind {
            Kind::Dir if node.links.is_empty() => {}
            Kind::Dir => out.dirs += 1,
            Kind::File => out.files += 1,
            Kind::Symlink => out.symlinks += 1,
        }
        if let Some(s) = node.stored.as_ref() {
            for x in s.xfields.iter().filter(|x| x.kind == INO_EXT_TYPE_DOCUMENT_ID) {
                let id = x
                    .data
                    .get(..4)
                    .and_then(|b| b.try_into().ok())
                    .map_or(0, u32::from_le_bytes);
                out.max_doc_id = out.max_doc_id.max(id);
            }
        }
        let dtype = match node.kind {
            Kind::Dir => DT_DIR,
            Kind::File => DT_REG,
            Kind::Symlink => DT_LNK,
        };
        for (l, link) in node.links.iter().enumerate() {
            let sibling = ids.siblings[i].get(l).copied();
            let named = Named {
                parent: tree.number(link.parent),
                name: &link.name,
                hash: link.hash,
                date_added: link.date_added,
            };
            fs.push(drec(input, &named, number, dtype, &drec_xfields(link, sibling))?);
            if let Some(sib) = sibling {
                fs.extend(sibling_records(tree, number, sib, link));
            }
        }
        for x in &node.xattrs {
            fs.push(xattr(input, &ids, number, x)?);
        }
    }

    let mut owned = Vec::new();
    for (s, placed) in input.extents.iter().enumerate() {
        let Some(id) = ids.stream[s] else {
            continue;
        };
        let users = &ids.data_users[s];
        if !users.is_empty() {
            fs.push((
                key(id, APFS_TYPE_DSTREAM_ID),
                Some(
                    DstreamIdVal {
                        refcnt: U32::new(users.len() as u32),
                    }
                    .as_bytes()
                    .to_vec(),
                ),
            ));
        }
        for e in placed {
            let k = FileExtentKey {
                hdr: JKey::new(id, APFS_TYPE_FILE_EXTENT),
                logical_addr: U64::new(e.logical),
            };
            let v = FileExtentVal {
                len_and_flags: U64::new(e.len | u64::from(e.flags) << 56),
                phys_block_num: U64::new(e.paddr),
                crypto_id: U64::new(e.crypto_id),
            };
            fs.push((k.as_bytes().to_vec(), Some(v.as_bytes().to_vec())));
            if e.paddr != 0 {
                owned.push(Owned {
                    paddr: e.paddr,
                    blocks: e.len.div_ceil(switch_apfs_format::BLOCK_SIZE as u64),
                    owner: ids.owner[s],
                });
            }
        }
    }
    if input.readonly_compatible_features & APFS_ROCOMPAT_CLONE_MAPPING != 0 {
        for group in &ids.clones {
            let size = clone_physical_size(tree, input.extents, group[0]);
            for &u in group {
                fs.push(clone_mapping(ids.private[u.0 as usize], tree.number(u), size));
                out.clone_mappings += 1;
            }
        }
    }
    keys::sort(fs);
    let refs = extents::build(&owned);
    out.extent_refs = refs.records;
    out.data_blocks = refs.blocks;
    out.shared_blocks = refs.shared_blocks;
    Ok(out)
}

fn key(oid: u64, kind: u8) -> Vec<u8> {
    JKey::new(oid, kind).as_bytes().to_vec()
}

fn drec_xfields(link: &Link, sibling: Option<u64>) -> Vec<XField> {
    let sibling = sibling.map(|id| XField {
        kind: DREC_EXT_TYPE_SIBLING_ID,
        flags: XF_DO_NOT_COPY,
        data: id.to_le_bytes().to_vec(),
    });
    sibling.into_iter().chain(link.xfields.iter().cloned()).collect()
}

struct Named<'a> {
    parent: u64,
    name: &'a str,
    hash: Option<u32>,
    date_added: Option<u64>,
}

fn drec(input: &RecordInput, link: &Named, file_id: u64, dtype: u16, xfields: &[XField]) -> Result<Record, WriteError> {
    let len_and_hash = match link.hash {
        Some(hash) => hash << 10 | (link.name.len() as u32 + 1),
        None => name_len_and_hash(link.name, input.unicode)?,
    };
    let mut k = key(link.parent, APFS_TYPE_DIR_REC);
    k.extend_from_slice(&len_and_hash.to_le_bytes());
    k.extend_from_slice(link.name.as_bytes());
    k.push(0);
    let v = DrecVal {
        file_id: U64::new(file_id),
        date_added: U64::new(link.date_added.unwrap_or(input.time_ns)),
        flags: U16::new(dtype),
    };
    let mut value = v.as_bytes().to_vec();
    value.extend_from_slice(&inode::xfields(xfields));
    Ok((k, Some(value)))
}

fn sibling_records(tree: &Tree, inode: u64, sibling: u64, link: &Link) -> [Record; 2] {
    let k = SiblingKey {
        hdr: JKey::new(inode, APFS_TYPE_SIBLING_LINK),
        sibling_id: U64::new(sibling),
    };
    let mut v = SiblingVal {
        parent_id: U64::new(tree.number(link.parent)),
        name_len: U16::new(link.name.len() as u16 + 1),
    }
    .as_bytes()
    .to_vec();
    v.extend_from_slice(link.name.as_bytes());
    v.push(0);
    [
        (k.as_bytes().to_vec(), Some(v)),
        (key(sibling, APFS_TYPE_SIBLING_MAP), Some(inode.to_le_bytes().to_vec())),
    ]
}

fn xattr(input: &RecordInput, ids: &Ids, number: u64, x: &crate::tree::Xattr) -> Result<Record, WriteError> {
    let mut k = key(number, APFS_TYPE_XATTR);
    k.extend_from_slice(&(x.name.len() as u16 + 1).to_le_bytes());
    k.extend_from_slice(x.name.as_bytes());
    k.push(0);
    let flags = x.flags & !(XATTR_DATA_STREAM | XATTR_DATA_EMBEDDED);
    let (flags, data) = match &x.value {
        XattrValue::Embedded(bytes) => (flags | XATTR_DATA_EMBEDDED, bytes.clone()),
        XattrValue::Stream(s) => {
            let i = s.0 as usize;
            let d = XattrDstream {
                xattr_obj_id: U64::new(ids.stream[i].unwrap_or_default()),
                dstream: inode::dstream(input.tree.stream(*s), &input.extents[i], false),
            };
            (flags | XATTR_DATA_STREAM, d.as_bytes().to_vec())
        }
    };
    let len = u16::try_from(data.len()).map_err(|_| WriteError::Spec(format!("xattr {} too long", x.name)))?;
    let mut v = XattrVal {
        flags: U16::new(flags),
        xdata_len: U16::new(len),
    }
    .as_bytes()
    .to_vec();
    v.extend_from_slice(&data);
    Ok((k, Some(v)))
}

/// What macOS 27.2 records as a clone's physical size: the shared data stream's allocated bytes, or a compressed
/// file's resource fork's. A clone whose decmpfs is in a stream of its own gets 0 (api_fixture_test has macOS clone
/// one and compares).
fn clone_physical_size(tree: &Tree, extents: &[Vec<PlacedExtent>], first: InodeRef) -> u64 {
    let node = tree.inode(first);
    let stream = node.data.or_else(|| {
        node.xattrs.iter().find_map(|x| match x.value {
            XattrValue::Stream(s) if x.name == RESOURCE_FORK_XATTR_NAME => Some(s),
            _ => None,
        })
    });
    stream.map_or(0, |s| physical_bytes(&extents[s.0 as usize]))
}

fn clone_mapping(private_id: u64, file_id: u64, physical_size: u64) -> Record {
    let k = CloneMappingKey {
        hdr: JKey::new(CLONE_MAPPING_OID, APFS_TYPE_CLONE_MAPPING),
        flags: U32::new(CLONE_MAPPING_FLAGS),
        private_id: U64::new(private_id),
        file_id: U64::new(file_id),
    };
    let v = CloneMappingVal {
        physical_size: U64::new(physical_size),
    };
    (k.as_bytes().to_vec(), Some(v.as_bytes().to_vec()))
}

/// The private directory every volume has, as macOS makes it: root's, mode 0644, empty.
fn private_dir(input: &RecordInput) -> Record {
    let node = Inode {
        kind: Kind::Dir,
        attrs: Attrs::new(0, 0, 0o644),
        xattrs: Vec::new(),
        data: None,
        links: Vec::new(),
        stored: None,
        number: PRIV_DIR_INO_NUM,
    };
    let value = inode::value(&InodeFields {
        inode: &node,
        parent: ROOT_DIR_PARENT,
        private_id: PRIV_DIR_INO_NUM,
        cloned: false,
        name: PRIV_DIR_NAME,
        nchildren_or_nlink: 0,
        uncompressed_size: 0,
        data: None,
        time_ns: input.time_ns,
    });
    (key(PRIV_DIR_INO_NUM, APFS_TYPE_INODE), Some(value))
}
