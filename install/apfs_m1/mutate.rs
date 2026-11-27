// SPDX-License-Identifier: GPL-3.0-or-later
//! Fixture edits and negative controls, each one block rewritten in place with its checksum.

use std::path::Path;

use switch_apfs_format::fs::{APFS_TYPE_INODE, JKey, ROOT_DIR_INO_NUM};
use switch_apfs_format::io::{BlockSource, BlockTarget, FileTarget};
use switch_apfs_format::spaceman::{SFQ_MAIN, SpacemanPhys};
use switch_apfs_format::volume::ApfsSuperblock;
use switch_apfs_format::{get, put, seal};
use switch_apfs_read::node::Node;
use switch_apfs_read::{Container, Omap, Volume};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U32};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(Clone, Copy, Debug, clap::ValueEnum)]
pub enum Mutation {
    /// The main free queue's node limit one past the size rule: the kernel attaches no volume.
    QueueLimit,
    /// The root directory's child count off by one, in the first fs-tree leaf, checksum fixed: fsck_apfs checks
    /// directory valence.
    DataLeaf,
}

fn volume(c: &Container, uuid: [u8; 16]) -> Result<&Volume> {
    c.volumes
        .iter()
        .find(|v| v.sb.vol_uuid == uuid)
        .ok_or_else(|| "no volume with that UUID".into())
}

/// newfs_apfs's role letters don't include System, so fixtures are made role-less and given it here.
pub fn set_role(image: &Path, uuid: [u8; 16], role: u16) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let v = volume(&c, uuid)?;
    let mut block = v.block.clone();
    let mut sb: ApfsSuperblock = get(&block[..], 0).ok_or("short superblock")?;
    sb.role = U16::new(role);
    put(&mut block[..], 0, &sb);
    seal(&mut block[..]);
    target.write_blocks(v.paddr, &block[..])?;
    target.flush()?;
    println!("volume {} at {}: role {role:#x}", v.oid, v.paddr);
    Ok(())
}

pub fn mutate(image: &Path, what: Mutation, uuid: Option<[u8; 16]>) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let (paddr, block) = match what {
        Mutation::QueueLimit => {
            let (paddr, block) = c.checkpoint.ephemeral(c.checkpoint.nx.spaceman_oid.get())?;
            let mut block = block.clone();
            let mut sm: SpacemanPhys = get(&block[..], 0).ok_or("short space manager")?;
            let limit = sm.fq[SFQ_MAIN].tree_node_limit.get();
            sm.fq[SFQ_MAIN].tree_node_limit = U16::new(limit + 1);
            put(&mut block[..], 0, &sm);
            (*paddr, block)
        }
        Mutation::DataLeaf => {
            let v = volume(&c, uuid.ok_or("--uuid names the volume")?)?;
            let omap = Omap::read(&target, v.sb.omap_oid.get())?;
            let mut oid = v.sb.root_tree_oid.get();
            let (paddr, mut block) = loop {
                let (_, node) = omap.lookup(oid, v.xid).ok_or("fs node unmapped")?;
                let paddr = node.paddr.get();
                let block = target.block(paddr)?;
                let node = Node::parse(&block[..], None)?;
                if node.is_leaf() {
                    break (paddr, block);
                }
                oid = node.child(0)?;
            };
            let at = {
                let node = Node::parse(&block[..], None)?;
                let want = JKey::new(ROOT_DIR_INO_NUM, APFS_TYPE_INODE);
                let i = (0..node.len())
                    .find(|&i| node.key(i).ok() == Some(want.as_bytes()))
                    .ok_or("no root inode")?;
                let value = node.value(i)?.ok_or("ghost inode")?;
                value.as_ptr() as usize - block.as_ptr() as usize + 56
            };
            let children = u32::from_le_bytes(block[at..at + 4].try_into()?);
            put(&mut block[..], at, &U32::new(children + 1));
            (paddr, block)
        }
    };
    let mut block = block;
    seal(&mut block[..]);
    target.write_blocks(paddr, &block[..])?;
    target.flush()?;
    println!("{what:?}: rewrote block {paddr}");
    Ok(())
}
