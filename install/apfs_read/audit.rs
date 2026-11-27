// SPDX-License-Identifier: GPL-3.0-or-later
//! The allocation audit: every block a reachable object uses, checked against the space manager's bitmap. Each used
//! block has exactly one owner, unless the extent-reference tree counts it as shared, and nothing lies outside the
//! container.

use std::collections::{BTreeMap, HashSet};
use std::fmt;

use switch_apfs_alloc::Bitmap;
use switch_apfs_format::fs::APFS_TYPE_FILE_EXTENT;
use switch_apfs_format::io::BlockSource;
use switch_apfs_format::spaceman::SFQ_MAIN;
use switch_apfs_format::{BLOCK_SIZE, header};

use crate::catalog::{self, FileExtent, PhysExtent};
use crate::fs::Fs;
use crate::{Container, ReadError};

const MAX_PROBLEMS: usize = 32;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Owner {
    BlockZero,
    CheckpointDescriptors,
    CheckpointData,
    InternalPool,
    PoolBitmapRing,
    ContainerOmap,
    QueuedFree,
    VolumeSuperblock(usize),
    VolumeOmap(usize),
    FsTree(usize),
    ExtentRefTree(usize),
    SnapMetaTree(usize),
    FextTree(usize),
    Integrity(usize),
    /// A data stream's extent: its stream id and logical byte offset.
    Data(usize, u64, u64),
    /// Mapped by the volume's object map, and reached from nothing else.
    MappedOnly(usize, u64),
}

impl Owner {
    fn volume(&self) -> Option<usize> {
        match *self {
            Owner::VolumeSuperblock(v)
            | Owner::VolumeOmap(v)
            | Owner::FsTree(v)
            | Owner::ExtentRefTree(v)
            | Owner::SnapMetaTree(v)
            | Owner::FextTree(v)
            | Owner::Integrity(v)
            | Owner::Data(v, ..)
            | Owner::MappedOnly(v, _) => Some(v),
            _ => None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Claim {
    pub start: u64,
    pub len: u64,
    pub owner: Owner,
}

#[derive(Clone, Debug)]
pub struct VolumeUse {
    pub index: usize,
    pub name: String,
    /// Blocks the volume's objects use, each counted once however many extents share it.
    pub owned: u64,
    pub fs_alloc_count: u64,
    pub shared_blocks: u64,
}

#[derive(Debug)]
pub struct Audit {
    /// In block order.
    pub claims: Vec<Claim>,
    pub blocks: u64,
    pub used: u64,
    pub owned: u64,
    pub queued: u64,
    pub volumes: Vec<VolumeUse>,
    pub problems: Vec<String>,
    pub problem_count: usize,
}

impl Audit {
    pub fn ok(&self) -> bool {
        self.problem_count == 0
    }

    fn problem(&mut self, what: String) {
        self.problem_count += 1;
        if self.problems.len() < MAX_PROBLEMS {
            self.problems.push(what);
        }
    }
}

impl fmt::Display for Audit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(
            f,
            "blocks {} used {} owned {} queued {} problems {}",
            self.blocks, self.used, self.owned, self.queued, self.problem_count
        )?;
        for v in &self.volumes {
            writeln!(
                f,
                "volume {} {}: owned {} fs_alloc_count {} shared {}",
                v.index, v.name, v.owned, v.fs_alloc_count, v.shared_blocks
            )?;
        }
        for p in &self.problems {
            writeln!(f, "problem: {p}")?;
        }
        Ok(())
    }
}

pub fn audit(c: &Container, src: &dyn BlockSource) -> Result<Audit, ReadError> {
    let nx = &c.checkpoint.nx;
    if nx.efi_jumpstart.get() != 0 || nx.keylocker.count.get() != 0 || nx.mkb_locker.count.get() != 0 {
        return Err(ReadError::Unsupported("EFI jumpstart or key lockers".into()));
    }
    let mut claims = container_claims(c);
    let mut extent_refs = BTreeMap::new();
    let mut volumes = Vec::new();
    for v in &c.volumes {
        let fs = Fs::open(src, v)?;
        if fs.sb.num_snapshots.get() != 0 || fs.sb.snap_meta_ext_oid.get() != 0 {
            return Err(ReadError::Unsupported(format!("volume {}: snapshots", v.index)));
        }
        claims.push(claim(v.paddr, 1, Owner::VolumeSuperblock(v.index)));
        let refs = volume_claims(&fs, v.index, &mut claims)?;
        volumes.push(VolumeUse {
            index: v.index,
            name: fs.sb.name(),
            owned: 0,
            fs_alloc_count: fs.sb.fs_alloc_count.get(),
            shared_blocks: refs.iter().filter(|p| p.refcnt > 1).map(|p| p.blocks).sum(),
        });
        extent_refs.insert(v.index, refs);
    }
    let mut audit = Audit {
        claims: Vec::new(),
        blocks: c.blocks(),
        used: c.space.used.count_set(0..c.blocks()),
        owned: 0,
        queued: c.space.queues[SFQ_MAIN].iter().map(|q| q.len).sum(),
        volumes,
        problems: Vec::new(),
        problem_count: 0,
    };
    let (owned, counted_twice) = overlaps(&mut audit, &mut claims, &extent_refs);
    audit.owned = owned.count_set(0..audit.blocks);
    for v in &mut audit.volumes {
        let claimed: u64 = claims
            .iter()
            .filter(|cl| cl.owner.volume() == Some(v.index))
            .map(|cl| cl.len)
            .sum();
        v.owned = claimed - counted_twice.get(&v.index).copied().unwrap_or(0);
    }
    against_bitmap(&mut audit, &owned, &c.space.used, src);
    audit.claims = claims;
    Ok(audit)
}

fn claim(start: u64, len: u64, owner: Owner) -> Claim {
    Claim { start, len, owner }
}

fn container_claims(c: &Container) -> Vec<Claim> {
    let cp = &c.checkpoint;
    let space = &c.space;
    let mut out = vec![
        claim(0, 1, Owner::BlockZero),
        claim(
            cp.desc_area.start,
            cp.desc_area.end - cp.desc_area.start,
            Owner::CheckpointDescriptors,
        ),
        claim(
            cp.data_area.start,
            cp.data_area.end - cp.data_area.start,
            Owner::CheckpointData,
        ),
        claim(space.pool.start, space.pool.end - space.pool.start, Owner::InternalPool),
        claim(
            space.pool_bitmap_ring.start,
            space.pool_bitmap_ring.end - space.pool_bitmap_ring.start,
            Owner::PoolBitmapRing,
        ),
        claim(c.omap.paddr, 1, Owner::ContainerOmap),
    ];
    out.extend(c.omap.nodes.iter().map(|&n| claim(n, 1, Owner::ContainerOmap)));
    out.extend(
        space.queues[SFQ_MAIN]
            .iter()
            .map(|q| claim(q.paddr, q.len, Owner::QueuedFree)),
    );
    out
}

/// The volume's claims; returns its extent-reference records, which say which data blocks are shared.
fn volume_claims(fs: &Fs, index: usize, out: &mut Vec<Claim>) -> Result<Vec<PhysExtent>, ReadError> {
    let mut nodes = Vec::new();
    let mut data = Vec::new();
    let visited = fs.records(&mut |key, value| {
        if catalog::key_header(key)?.1 == APFS_TYPE_FILE_EXTENT {
            data.push(FileExtent::decode(key, value)?);
        }
        Ok(())
    })?;
    nodes.extend(visited.nodes.iter().map(|&n| claim(n, 1, Owner::FsTree(index))));
    let fext = fs.fext_scan(None, &mut |key, value| {
        data.push(FileExtent::decode_fext(key, value.unwrap_or_default())?);
        Ok(())
    })?;
    nodes.extend(fext.nodes.iter().map(|&n| claim(n, 1, Owner::FextTree(index))));
    let mut phys = Vec::new();
    let er = fs.physical_tree(fs.sb.extentref_tree_oid.get(), &mut |key, value| {
        phys.push(PhysExtent::decode(key, value.unwrap_or_default())?);
        Ok(())
    })?;
    nodes.extend(er.nodes.iter().map(|&n| claim(n, 1, Owner::ExtentRefTree(index))));
    let snap = fs.physical_tree(fs.sb.snap_meta_tree_oid.get(), &mut |_, _| Ok(()))?;
    nodes.extend(snap.nodes.iter().map(|&n| claim(n, 1, Owner::SnapMetaTree(index))));
    nodes.push(claim(fs.omap.paddr, 1, Owner::VolumeOmap(index)));
    nodes.extend(fs.omap.nodes.iter().map(|&n| claim(n, 1, Owner::VolumeOmap(index))));
    if let Some(seal) = fs.seal {
        nodes.push(claim(seal.integrity_paddr, 1, Owner::Integrity(index)));
    }
    let reached: HashSet<u64> = nodes.iter().map(|n| n.start).collect();
    for (k, v) in &fs.omap.entries {
        let p = v.paddr.get();
        if !reached.contains(&p) {
            let blocks = u64::from(v.size.get()).div_ceil(BLOCK_SIZE as u64);
            out.push(claim(p, blocks, Owner::MappedOnly(index, k.oid.get())));
        }
    }
    out.extend(nodes);
    out.extend(data.iter().filter(|e| e.phys_block_num != 0).map(|e| {
        claim(
            e.phys_block_num,
            e.blocks(),
            Owner::Data(index, e.stream, e.logical_addr),
        )
    }));
    phys.sort_by_key(|p| p.paddr);
    Ok(phys)
}

type ExtentRefs = BTreeMap<usize, Vec<PhysExtent>>;

/// Sorts the claims and reports every block claimed twice, except data blocks its extent-reference record shares.
/// Returns the owned blocks, and per volume how many shared blocks its claims count more than once.
fn overlaps(audit: &mut Audit, claims: &mut [Claim], extent_refs: &ExtentRefs) -> (Bitmap, BTreeMap<usize, u64>) {
    claims.sort_by_key(|c| (c.start, c.len));
    let mut owned = Bitmap::new(audit.blocks);
    let mut counted_twice = BTreeMap::new();
    let mut reach: Option<&Claim> = None;
    for c in claims.iter() {
        if c.len == 0 || c.start + c.len > audit.blocks {
            audit.problem(format!(
                "{:?} claims {}+{}, outside the container",
                c.owner, c.start, c.len
            ));
            continue;
        }
        if let Some(r) = reach
            && c.start < r.start + r.len
        {
            let end = (r.start + r.len).min(c.start + c.len);
            match shared_data(r, c, extent_refs) {
                Some(v) => *counted_twice.entry(v).or_insert(0) += end - c.start,
                None => audit.problem(format!(
                    "{:?} at {}+{} overlaps {:?} at {}+{}",
                    c.owner, c.start, c.len, r.owner, r.start, r.len
                )),
            }
        }
        if reach.is_none_or(|r| c.start + c.len > r.start + r.len) {
            reach = Some(c);
        }
        owned.set_range(c.start..c.start + c.len);
    }
    (owned, counted_twice)
}

/// Two data extents of one volume may share blocks when one extent-reference record covers the shared run and counts
/// more than one reference to it. Returns that volume.
fn shared_data(a: &Claim, b: &Claim, extent_refs: &ExtentRefs) -> Option<usize> {
    let (Owner::Data(va, ..), Owner::Data(vb, ..)) = (&a.owner, &b.owner) else {
        return None;
    };
    let end = (a.start + a.len).min(b.start + b.len);
    let refs = extent_refs.get(va)?;
    let i = refs.partition_point(|p| p.paddr <= b.start);
    let p = refs.get(i.checked_sub(1)?)?;
    (va == vb && p.refcnt > 1 && end <= p.paddr + p.blocks).then_some(*va)
}

fn against_bitmap(audit: &mut Audit, owned: &Bitmap, used: &Bitmap, src: &dyn BlockSource) {
    let mut i = 0;
    while i < audit.blocks {
        let (u, o) = (used.get(i), owned.get(i));
        if u == o {
            i += 1;
            continue;
        }
        let start = i;
        while i < audit.blocks && used.get(i) == u && owned.get(i) == o {
            i += 1;
        }
        audit.problem(if u {
            let kind = src
                .block(start)
                .map(|b| header(&b[..]).type_and_flags.get())
                .unwrap_or_default();
            format!("blocks {start}..{i} are in use and nothing owns them (the first's object type {kind:#x})")
        } else {
            format!("blocks {start}..{i} are owned and free in the bitmap")
        });
    }
}
