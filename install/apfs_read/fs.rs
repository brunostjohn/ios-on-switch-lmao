// SPDX-License-Identifier: GPL-3.0-or-later
//! One volume's file system: its object map, its fs tree (plain, or hashed and headerless when sealed), the fext
//! tree of a sealed volume, and record lookups by object id.

use std::cmp::Ordering;
use std::collections::{HashMap, VecDeque};
use std::sync::Mutex;

use switch_apfs_format::btree::BTNODE_NOHEADER;
use switch_apfs_format::fs::{APFS_TYPE_DIR_REC, APFS_TYPE_FILE_EXTENT, APFS_TYPE_INODE, APFS_TYPE_XATTR};
use switch_apfs_format::fs_records::{
    APFS_HASH_SHA256, APFS_HASH_SHA256_SIZE, APFS_TYPE_FILE_INFO, IntegrityMetaPhys, TYPE_INTEGRITY_META,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::{OBJ_PHYSICAL, OBJ_VIRTUAL, TYPE_BTREE};
use switch_apfs_format::omap::OMAP_VAL_NOHEADER;
use switch_apfs_format::volume::{
    APFS_FS_UNENCRYPTED, APFS_INCOMPAT_CASE_INSENSITIVE, APFS_INCOMPAT_NORMALIZATION_INSENSITIVE,
    APFS_INCOMPAT_SEALED_VOLUME, ApfsSuperblock,
};
use switch_apfs_format::{BLOCK_SIZE, Block, checksum_ok, get};

use crate::catalog::{self, DirRec, FileExtent, FileInfo, Inode, Xattr};
use crate::omap::{Omap, Volume};
use crate::walk::{self, NodeReader, Physical, Tree, Visit, Visited};
use crate::{ReadError, invalid};

/// Enough for every index node of Apple's System volume and a working set of leaves.
const CACHE_BLOCKS: usize = 8192;
const OMAP_VAL_ENCRYPTED: u32 = 0x2;

#[derive(Clone, Copy, Debug)]
pub struct Seal {
    pub integrity_oid: u64,
    pub integrity_paddr: u64,
    pub meta: IntegrityMetaPhys,
    pub root_hash: [u8; 32],
    pub fext_root: u64,
}

#[derive(Clone, Debug)]
pub struct RawRecord {
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}

impl RawRecord {
    pub fn header(&self) -> (u64, u8) {
        catalog::key_header(&self.key).unwrap_or_default()
    }
}

pub struct Fs<'a> {
    src: &'a dyn BlockSource,
    pub sb: ApfsSuperblock,
    pub xid: u64,
    pub omap: Omap,
    pub seal: Option<Seal>,
    cache: Mutex<Cache>,
}

#[derive(Default)]
struct Cache {
    blocks: HashMap<u64, Box<Block>>,
    order: VecDeque<u64>,
}

impl<'a> Fs<'a> {
    pub fn open(src: &'a dyn BlockSource, volume: &Volume) -> Result<Self, ReadError> {
        let sb = volume.sb;
        let what = format!("volume {} ({})", volume.oid, sb.name());
        if sb.fs_flags.get() & APFS_FS_UNENCRYPTED == 0 || sb.er_state_oid.get() != 0 {
            return Err(ReadError::Unsupported(format!("{what}: encrypted")));
        }
        if sb.root_tree_type.get() != OBJ_VIRTUAL | TYPE_BTREE {
            return Err(ReadError::Unsupported(format!(
                "{what}: fs-tree type {:#x}",
                sb.root_tree_type.get()
            )));
        }
        let omap = Omap::read(src, sb.omap_oid.get())?;
        let mut fs = Self {
            src,
            sb,
            xid: volume.xid,
            omap,
            seal: None,
            cache: Mutex::default(),
        };
        fs.seal = fs.read_seal().map_err(|e| invalid!("{what}: {e}"))?;
        Ok(fs)
    }

    pub fn source(&self) -> &'a dyn BlockSource {
        self.src
    }

    /// Whether directory keys carry a name hash (`j_drec_hashed_key_t`).
    pub fn hashed_names(&self) -> bool {
        self.sb.incompatible_features.get() & (APFS_INCOMPAT_CASE_INSENSITIVE | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)
            != 0
    }

    fn read_seal(&self) -> Result<Option<Seal>, ReadError> {
        let sealed = self.sb.incompatible_features.get() & APFS_INCOMPAT_SEALED_VOLUME != 0;
        let oid = self.sb.integrity_meta_oid.get();
        match (sealed, oid) {
            (false, 0) => return Ok(None),
            (false, _) | (true, 0) => return Err(invalid!("the sealed bit and the integrity object disagree")),
            (true, _) => {}
        }
        let (_, val) = self
            .omap
            .lookup(oid, self.xid)
            .ok_or_else(|| invalid!("integrity object {oid} unmapped"))?;
        let paddr = val.paddr.get();
        let block = self.src.block(paddr)?;
        let meta: IntegrityMetaPhys = get(&block[..], 0).unwrap_or_default();
        let at = meta.root_hash_offset.get() as usize;
        if !checksum_ok(&block[..])
            || meta.o.type_and_flags.get() != OBJ_VIRTUAL | TYPE_INTEGRITY_META
            || meta.o.oid.get() != oid
            || at < size_of::<IntegrityMetaPhys>()
            || at + APFS_HASH_SHA256_SIZE > BLOCK_SIZE
        {
            return Err(invalid!("integrity object {oid} at {paddr}"));
        }
        if meta.hash_type.get() != APFS_HASH_SHA256 {
            return Err(ReadError::Unsupported(format!(
                "seal hash type {}",
                meta.hash_type.get()
            )));
        }
        if meta.broken_xid.get() != 0 {
            return Err(invalid!("the seal was broken at xid {}", meta.broken_xid.get()));
        }
        if self.sb.fext_tree_type.get() != OBJ_PHYSICAL | TYPE_BTREE || self.sb.fext_tree_oid.get() == 0 {
            return Err(invalid!("sealed, without a physical fext tree"));
        }
        Ok(Some(Seal {
            integrity_oid: oid,
            integrity_paddr: paddr,
            meta,
            root_hash: block[at..at + APFS_HASH_SHA256_SIZE].try_into().unwrap_or_default(),
            fext_root: self.sb.fext_tree_oid.get(),
        }))
    }

    fn fs_tree(&self) -> Tree<'_> {
        Tree {
            reader: self,
            root: self.sb.root_tree_oid.get(),
            root_hash: self.seal.map(|s| s.root_hash),
        }
    }

    /// Every fs-tree record in key order, with the visited nodes' addresses.
    pub fn records(&self, visit: &mut dyn FnMut(&[u8], &[u8]) -> Result<(), ReadError>) -> Result<Visited, ReadError> {
        let mut last = (0u64, 0u8);
        walk::scan(&self.fs_tree(), None, &mut |key, value| {
            let here = catalog::key_header(key)?;
            if here < last {
                return Err(invalid!("fs-tree record {here:?} after {last:?}"));
            }
            last = here;
            visit(key, value.ok_or_else(|| invalid!("a ghost in the fs tree"))?)
        })
    }

    /// The records of one object, of one record type or of all.
    pub fn object(&self, oid: u64, kind: Option<u8>) -> Result<Vec<RawRecord>, ReadError> {
        let mut out = Vec::new();
        let range = move |key: &[u8]| catalog::order(key, oid, kind);
        walk::scan(&self.fs_tree(), Some(&range), &mut |key, value| {
            out.push(RawRecord {
                key: key.to_vec(),
                value: value.ok_or_else(|| invalid!("a ghost in the fs tree"))?.to_vec(),
            });
            Ok(())
        })?;
        Ok(out)
    }

    pub fn inode(&self, oid: u64) -> Result<Option<Inode>, ReadError> {
        let records = self.object(oid, Some(APFS_TYPE_INODE))?;
        match records.as_slice() {
            [] => Ok(None),
            [r] => Inode::decode(oid, &r.value).map(Some),
            _ => Err(invalid!("{} inode records for {oid}", records.len())),
        }
    }

    pub fn children(&self, dir: u64) -> Result<Vec<DirRec>, ReadError> {
        let hashed = self.hashed_names();
        self.object(dir, Some(APFS_TYPE_DIR_REC))?
            .iter()
            .map(|r| DirRec::decode(&r.key, &r.value, hashed))
            .collect()
    }

    pub fn xattrs(&self, oid: u64) -> Result<Vec<Xattr>, ReadError> {
        self.object(oid, Some(APFS_TYPE_XATTR))?
            .iter()
            .map(|r| Xattr::decode(&r.key, &r.value))
            .collect()
    }

    /// A data stream's extents in logical order: from the fext tree on a sealed volume, from the fs tree otherwise.
    pub fn extents(&self, stream: u64) -> Result<Vec<FileExtent>, ReadError> {
        if self.seal.is_none() {
            return self
                .object(stream, Some(APFS_TYPE_FILE_EXTENT))?
                .iter()
                .map(|r| FileExtent::decode(&r.key, &r.value))
                .collect();
        }
        let mut out = Vec::new();
        let range = move |key: &[u8]| -> Result<Ordering, ReadError> {
            let id = u64::from_le_bytes(key.get(..8).and_then(|k| k.try_into().ok()).unwrap_or_default());
            Ok(id.cmp(&stream))
        };
        self.fext_scan(Some(&range), &mut |key, value| {
            out.push(FileExtent::decode_fext(
                key,
                value.ok_or_else(|| invalid!("a ghost in the fext tree"))?,
            )?);
            Ok(())
        })?;
        Ok(out)
    }

    pub fn fext_scan(&self, range: Option<&walk::Range>, visit: &mut Visit) -> Result<Visited, ReadError> {
        let Some(seal) = self.seal else {
            return Ok(Visited {
                nodes: Vec::new(),
                records: 0,
            });
        };
        let tree = Tree {
            reader: &Physical(self.src),
            root: seal.fext_root,
            root_hash: None,
        };
        walk::scan(&tree, range, visit)
    }

    pub fn file_info(&self, stream: u64) -> Result<Vec<FileInfo>, ReadError> {
        self.object(stream, Some(APFS_TYPE_FILE_INFO))?
            .iter()
            .map(|r| FileInfo::decode(&r.key, &r.value))
            .collect()
    }

    /// A physical tree of the volume (extent references or snapshot metadata), every record in key order.
    pub fn physical_tree(&self, root: u64, visit: &mut Visit) -> Result<Visited, ReadError> {
        if root == 0 {
            return Ok(Visited {
                nodes: Vec::new(),
                records: 0,
            });
        }
        walk::walk(&Physical(self.src), root, visit)
    }

    /// The inode a path names, from the root directory. Each component is found by scanning its directory's
    /// records, so nothing depends on the name hash.
    pub fn lookup(&self, path: &str) -> Result<Option<u64>, ReadError> {
        let mut oid = switch_apfs_format::fs::ROOT_DIR_INO_NUM;
        for part in path.split('/').filter(|p| !p.is_empty()) {
            match self.children(oid)?.into_iter().find(|d| d.name == part) {
                Some(d) => oid = d.file_id(),
                None => return Ok(None),
            }
        }
        Ok(Some(oid))
    }
}

impl NodeReader for Fs<'_> {
    fn node(&self, oid: u64) -> Result<(u64, Box<Block>), ReadError> {
        let (_, val) = self
            .omap
            .lookup(oid, self.xid)
            .ok_or_else(|| invalid!("fs-tree node {oid} unmapped"))?;
        let (paddr, flags) = (val.paddr.get(), val.flags.get());
        if val.size.get() as usize != BLOCK_SIZE || flags & OMAP_VAL_ENCRYPTED != 0 {
            return Err(ReadError::Unsupported(format!(
                "fs-tree node {oid}: object-map flags {flags:#x}"
            )));
        }
        let block = self.cached(paddr)?;
        let headerless = u16::from_le_bytes([block[32], block[33]]) & BTNODE_NOHEADER != 0;
        if headerless != (flags & OMAP_VAL_NOHEADER != 0) {
            return Err(invalid!(
                "fs-tree node {oid} at {paddr}: NOHEADER disagrees with its object map"
            ));
        }
        Ok((paddr, block))
    }
}

impl Fs<'_> {
    fn cached(&self, paddr: u64) -> Result<Box<Block>, ReadError> {
        let mut cache = self.cache.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(block) = cache.blocks.get(&paddr) {
            return Ok(block.clone());
        }
        let block = self.src.block(paddr)?;
        if cache.order.len() == CACHE_BLOCKS
            && let Some(old) = cache.order.pop_front()
        {
            cache.blocks.remove(&old);
        }
        cache.order.push_back(paddr);
        cache.blocks.insert(paddr, block.clone());
        Ok(block)
    }
}
