// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_format::volume::ApfsSuperblock;
use switch_apfs_format::{get, put, seal};
use switch_apfs_read::Container;
use zerocopy::little_endian::U16;

use crate::parse::uuid_string;
use crate::{Error, Result};

pub fn inspect(image: &Path, offset: u64) -> Result<()> {
    let src = FileSource::open(image, offset)?;
    let c = Container::open(&src)?;
    println!(
        "container blocks {} free {} xid {} volumes {}",
        c.blocks(),
        c.space.free_count,
        c.checkpoint.xid(),
        c.volumes.len()
    );
    for v in &c.volumes {
        println!(
            "volume {} oid {} uuid {} role {:#x} rocompat {:#x} incompat {:#x} name {}",
            v.index,
            v.oid,
            uuid_string(&v.sb.vol_uuid),
            v.sb.role.get(),
            v.sb.readonly_compatible_features.get(),
            v.sb.incompatible_features.get(),
            v.sb.name()
        );
    }
    Ok(())
}

pub fn set_role(image: &Path, uuid: [u8; 16], role: u16) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let v = c
        .volume(uuid)
        .ok_or_else(|| Error::Check(format!("no volume with UUID {}", uuid_string(&uuid))))?;
    let mut block = v.block.clone();
    let mut sb: ApfsSuperblock = get(&block[..], 0).ok_or_else(|| Error::Check("short superblock".into()))?;
    sb.role = U16::new(role);
    put(&mut block[..], 0, &sb);
    seal(&mut block[..]);
    target.write_blocks(v.paddr, &block[..])?;
    target.flush()?;
    println!("volume {} at {}: role {role:#x}", v.oid, v.paddr);
    Ok(())
}
