// SPDX-License-Identifier: GPL-3.0-or-later
//! Surgical image mutations for negative-control oracle checks.

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use switch_apfs_format::fs_records::RESOURCE_FORK_XATTR_NAME;
use switch_apfs_format::io::{BlockSource, BlockTarget, FileSource, FileTarget};
use switch_apfs_format::spaceman::{SFQ_MAIN, SpacemanPhys};
use switch_apfs_format::volume::{APFS_VOL_ROLE_DATA, APFS_VOL_ROLE_PREBOOT, ApfsSuperblock};
use switch_apfs_format::{BLOCK_SIZE, get, put, seal};
use switch_apfs_read::node::Node;
use switch_apfs_read::{Container, Fs, Omap, Volume, XattrData};
use switch_image::STAGING_DIR;
use zerocopy::little_endian::U16;

use crate::parse::uuid_string;
use crate::{Error, Result};

/// Increments the main free-queue node limit by 1 and re-seals the spaceman checksum.
pub fn queue_limit(image: &Path) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let (paddr, block) = c.checkpoint.ephemeral(c.checkpoint.nx.spaceman_oid.get())?;
    let paddr = *paddr;
    let mut block = block.clone();
    let mut sm: SpacemanPhys = get(&block[..], 0).ok_or_else(|| Error::Check("short space manager".into()))?;
    let old = sm.fq[SFQ_MAIN].tree_node_limit.get();
    sm.fq[SFQ_MAIN].tree_node_limit = U16::new(old + 1);
    put(&mut block[..], 0, &sm);
    seal(&mut block[..]);
    target.write_blocks(paddr, &block[..])?;
    target.flush()?;
    println!("tamper queue-limit: block {paddr}, main limit {old} -> {}", old + 1);
    Ok(())
}

/// Flips one raw byte in a Data fs-tree leaf block without updating its checksum so `fsck_apfs -n` fails.
pub fn data_leaf(image: &Path, data_uuid: Option<[u8; 16]>) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let v = pick_volume(&c, data_uuid, APFS_VOL_ROLE_DATA, "Data")?;
    let omap = Omap::read(&target, v.sb.omap_oid.get())?;
    let mut oid = v.sb.root_tree_oid.get();
    let (paddr, mut block) = loop {
        let (_, mapped) = omap
            .lookup(oid, v.xid)
            .ok_or_else(|| Error::Check(format!("fs node {oid} unmapped")))?;
        let paddr = mapped.paddr.get();
        let block = target.block(paddr)?;
        let node = Node::parse(&block[..], None)?;
        if node.is_leaf() {
            break (paddr, block);
        }
        oid = node.child(0)?;
    };
    const FLIP_OFFSET: usize = 100;
    block[FLIP_OFFSET] ^= 0xFF;
    target.write_blocks(paddr, &block[..])?;
    target.flush()?;
    println!("tamper data-leaf: flipped block {paddr} byte {FLIP_OFFSET}");
    Ok(())
}

/// Flips one data byte of a file inside a sealed cryptex (either inside Preboot's single extent or standalone).
pub fn preboot_cryptex_byte(
    image: &Path,
    preboot_uuid: Option<[u8; 16]>,
    cryptex_name: Option<&str>,
    cryptex_path: &str,
    standalone: bool,
) -> Result<()> {
    let outer_offset = if standalone {
        0u64
    } else {
        let name = cryptex_name.ok_or_else(|| Error::Check("--cryptex-name is required unless --standalone".into()))?;
        let src = FileSource::open(image, 0)?;
        let c = Container::open(&src)?;
        let preboot = pick_volume(&c, preboot_uuid, APFS_VOL_ROLE_PREBOOT, "Preboot")?;
        let fs = Fs::open(&src, preboot)?;
        let full_path = format!("/{STAGING_DIR}/{name}");
        let oid = fs
            .lookup(&full_path)?
            .ok_or_else(|| Error::Check(format!("{full_path} missing in Preboot")))?;
        let (entry, _) = fs.entry(oid, full_path.clone(), None)?;
        let stream = entry
            .data
            .ok_or_else(|| Error::Check(format!("{full_path} has no data stream")))?;
        if stream.extents.len() != 1 {
            return Err(Error::Check(format!(
                "{full_path}: expected 1 extent, got {}",
                stream.extents.len()
            )));
        }
        stream.extents[0].phys_block_num * BLOCK_SIZE as u64
    };

    let inner_paddr = {
        let inner_src = FileSource::open(image, outer_offset)?;
        let inner_c = Container::open(&inner_src)?;
        let vol = inner_c
            .volumes
            .first()
            .ok_or_else(|| Error::Check("sealed cryptex has no volumes".into()))?;
        let fs = Fs::open(&inner_src, vol)?;
        let oid = fs
            .lookup(cryptex_path)?
            .ok_or_else(|| Error::Check(format!("{cryptex_path} missing in cryptex")))?;
        let inode = fs
            .inode(oid)?
            .ok_or_else(|| Error::Check(format!("inode {oid} missing in cryptex")))?;
        let stream_id = if inode.is_compressed() {
            fs.xattrs(oid)?
                .into_iter()
                .find_map(|x| match x.data {
                    XattrData::Stream { id, .. } if x.name == RESOURCE_FORK_XATTR_NAME => Some(id),
                    _ => None,
                })
                .ok_or_else(|| Error::Check(format!("{cryptex_path}: compressed without resource fork")))?
        } else {
            inode.val.private_id.get()
        };
        let extents = fs.extents(stream_id)?;
        let first = extents
            .first()
            .ok_or_else(|| Error::Check(format!("{cryptex_path} has no data extents")))?;
        first.phys_block_num
    };

    let pos = outer_offset + inner_paddr * BLOCK_SIZE as u64;
    let mut file = OpenOptions::new().read(true).write(true).open(image)?;
    file.seek(SeekFrom::Start(pos))?;
    let mut byte = [0u8; 1];
    file.read_exact(&mut byte)?;
    byte[0] ^= 0xFF;
    file.seek(SeekFrom::Start(pos))?;
    file.write_all(&byte)?;
    file.sync_data()?;
    println!("tamper preboot-cryptex-byte: flipped byte at file offset {pos} (inner block {inner_paddr})");
    Ok(())
}

/// Rewrites the Data volume superblock's UUID and re-seals its checksum so `fsck_apfs` passes while pin checks fail.
pub fn wrong_data_uuid(image: &Path, data_uuid: Option<[u8; 16]>, new_uuid: Option<[u8; 16]>) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let v = pick_volume(&c, data_uuid, APFS_VOL_ROLE_DATA, "Data")?;
    let mut block = v.block.clone();
    let mut sb: ApfsSuperblock = get(&block[..], 0).ok_or_else(|| Error::Check("short superblock".into()))?;
    let old = sb.vol_uuid;
    let replacement = new_uuid.unwrap_or_else(|| {
        let mut flipped = old;
        flipped[0] ^= 0xFF;
        flipped
    });
    if replacement == old {
        return Err(Error::Check("replacement UUID equals existing Data UUID".into()));
    }
    sb.vol_uuid = replacement;
    put(&mut block[..], 0, &sb);
    seal(&mut block[..]);
    target.write_blocks(v.paddr, &block[..])?;
    target.flush()?;
    println!(
        "tamper wrong-data-uuid: block {}, {} -> {}",
        v.paddr,
        uuid_string(&old),
        uuid_string(&replacement)
    );
    Ok(())
}

fn pick_volume<'c>(c: &'c Container, uuid: Option<[u8; 16]>, role: u16, label: &str) -> Result<&'c Volume> {
    let found = match uuid {
        Some(u) => c.volume(u),
        None => c.volumes.iter().find(|v| v.sb.role.get() == role),
    };
    found.ok_or_else(|| Error::Check(format!("container has no {label} volume")))
}
