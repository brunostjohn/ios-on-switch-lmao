// SPDX-License-Identifier: GPL-3.0-or-later
//! Object maps and the volumes the container's object map names.

use switch_apfs_format::io::BlockSource;
use switch_apfs_format::object::{OBJ_PHYSICAL, OBJ_VIRTUAL, TYPE_FS, TYPE_OMAP};
use switch_apfs_format::omap::{OMAP_VAL_DELETED, OmapKey, OmapPhys, OmapVal};
use switch_apfs_format::volume::{APFS_MAGIC, ApfsSuperblock};
use switch_apfs_format::{BLOCK_SIZE, Block, checksum_ok, get};

use crate::ReadError;
use crate::walk::{Physical, walk};

pub struct Omap {
    pub paddr: u64,
    pub phys: OmapPhys,
    /// In key order: by oid, then xid.
    pub entries: Vec<(OmapKey, OmapVal)>,
    pub nodes: Vec<u64>,
}

impl Omap {
    pub fn read(src: &dyn BlockSource, paddr: u64) -> Result<Self, ReadError> {
        let block = src.block(paddr)?;
        let phys: OmapPhys = get(&block[..], 0).ok_or_else(|| ReadError::invalid("short object map"))?;
        if !checksum_ok(&block[..]) || phys.o.type_and_flags.get() != OBJ_PHYSICAL | TYPE_OMAP {
            return Err(ReadError::invalid(format!("object map at {paddr}")));
        }
        let mut entries = Vec::new();
        let visited = walk(&Physical(src), phys.tree_oid.get(), &mut |key, value| {
            let key: OmapKey = get(key, 0).ok_or_else(|| ReadError::invalid("short object-map key"))?;
            let value = value.ok_or_else(|| ReadError::invalid("ghost in an object map"))?;
            entries.push((
                key,
                get(value, 0).ok_or_else(|| ReadError::invalid("short object-map value"))?,
            ));
            Ok(())
        })?;
        Ok(Self {
            paddr,
            phys,
            entries,
            nodes: visited.nodes,
        })
    }

    /// The newest mapping of `oid` no newer than `xid`, as a mount at checkpoint `xid` sees it.
    pub fn lookup(&self, oid: u64, xid: u64) -> Option<(OmapKey, OmapVal)> {
        let start = self.entries.partition_point(|(k, _)| k.oid.get() < oid);
        self.entries[start..]
            .iter()
            .take_while(|(k, _)| k.oid.get() == oid)
            .filter(|(k, _)| k.xid.get() <= xid)
            .last()
            .filter(|(_, v)| v.flags.get() & OMAP_VAL_DELETED == 0)
            .copied()
    }
}

pub struct Volume {
    pub index: usize,
    pub oid: u64,
    pub xid: u64,
    pub paddr: u64,
    pub sb: ApfsSuperblock,
    pub block: Box<Block>,
}

pub fn volumes(
    src: &dyn BlockSource,
    nx: &switch_apfs_format::container::NxSuperblock,
    omap: &Omap,
) -> Result<Vec<Volume>, ReadError> {
    let xid = nx.o.xid.get();
    let mut out = Vec::new();
    for (index, oid) in nx.volumes() {
        let (key, value) = omap
            .lookup(oid, xid)
            .ok_or_else(|| ReadError::invalid(format!("volume {oid} unmapped")))?;
        if value.size.get() as usize != BLOCK_SIZE || value.flags.get() != 0 {
            return Err(ReadError::Unsupported(format!(
                "volume {oid}: object-map flags {:#x}",
                value.flags.get()
            )));
        }
        let paddr = value.paddr.get();
        let block = src.block(paddr)?;
        let sb: ApfsSuperblock = get(&block[..], 0).ok_or_else(|| ReadError::invalid("short volume superblock"))?;
        if !checksum_ok(&block[..])
            || sb.magic.get() != APFS_MAGIC
            || sb.o.oid.get() != oid
            || sb.o.xid.get() != key.xid.get()
            || sb.o.type_and_flags.get() != OBJ_VIRTUAL | TYPE_FS
            || sb.fs_index.get() as usize != index
        {
            return Err(ReadError::invalid(format!("volume {oid} superblock at {paddr}")));
        }
        out.push(Volume {
            index,
            oid,
            xid: key.xid.get(),
            paddr,
            sb,
            block,
        });
    }
    Ok(out)
}
