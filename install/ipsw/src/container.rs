// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Read, Seek, SeekFrom, Write};

use switch_catalog::{Container, Member};
use udif::{DmgReader, DmgReaderOptions};

use crate::error::{Error, Result};
use crate::extract::copy;
use crate::pipe::{Sink, Written, hashed};

/// UDIF's block maps count 512-byte sectors whatever the disk's own block size.
const SECTOR: u64 = 512;
const KOLY: &[u8; 4] = b"koly";
const SECTOR_COUNT_AT: i64 = 0x1ec;
const NXSB: &[u8; 4] = b"NXSB";
const NXSB_AT: u64 = 0x20;

/// UDIF by its trailer, else APFS by the container superblock's magic.
pub fn sniff<R: Read + Seek>(image: &mut R) -> io::Result<Option<Container>> {
    let len = image.seek(SeekFrom::End(0))?;
    let mut magic = [0u8; 4];
    if len >= SECTOR {
        image.seek(SeekFrom::End(-(SECTOR as i64)))?;
        image.read_exact(&mut magic)?;
        if &magic == KOLY {
            return Ok(Some(Container::Udif));
        }
    }
    if len >= NXSB_AT + 4 {
        image.seek(SeekFrom::Start(NXSB_AT))?;
        image.read_exact(&mut magic)?;
        if &magic == NXSB {
            return Ok(Some(Container::Apfs));
        }
    }
    Ok(None)
}

/// The APFS container of an OS image whose format the catalog gives as `declared`, written to `out`. The member's own
/// bytes must agree with `declared`.
pub fn os_container<R: Read + Seek, W: Write + Send>(
    mut image: R,
    out: &mut W,
    declared: Container,
) -> Result<Written> {
    let found = sniff(&mut image)?;
    if found != Some(declared) {
        return Err(Error::Container { declared, found });
    }
    let written = match declared {
        Container::Apfs => {
            image.seek(SeekFrom::Start(0))?;
            hashed(out, |sink| copy(&mut image, sink))?.1
        }
        Container::Udif => {
            let mut dmg = open_udif(image)?;
            let id = apfs_partition(&dmg)?;
            hashed(out, |sink| Ok(dmg.decompress_partition_to(id, sink)?))?.1
        }
    };
    Ok(written)
}

/// The APFS container of a disk image member's content, written to `out`, refused unless it is the one the member's
/// catalog pins give.
pub fn member_container<R: Read + Seek, W: Write + Send>(image: R, member: &Member, out: &mut W) -> Result<Written> {
    let pins = member
        .container
        .as_ref()
        .ok_or_else(|| Error::NoContainer(member.member.clone()))?;
    let declared = Container::from_name(&pins.format).ok_or_else(|| Error::NoContainer(member.member.clone()))?;
    let written = os_container(image, out, declared)?;
    if written.len != pins.size || written.sha256 != pins.sha256.0 {
        return Err(Error::ContainerContent {
            member: member.member.clone(),
            got_len: written.len,
            got: hex::encode(written.sha256),
            expected_len: pins.size,
            expected: pins.sha256.to_string(),
        });
    }
    Ok(written)
}

/// The whole disk a UDIF image holds, as `hdiutil convert -format UDTO` writes it.
pub fn udif_disk<R: Read + Seek, W: Write + Send>(mut image: R, out: &mut W) -> Result<Written> {
    let total = sector_count(&mut image)? * SECTOR;
    let mut dmg = open_udif(image)?;
    let mut partitions: Vec<(u64, u64, i32)> = dmg
        .partitions()
        .iter()
        .map(|p| (p.block_map.first_sector, p.block_map.sector_count, p.id))
        .collect();
    partitions.sort_unstable();
    let (_, written) = hashed(out, |sink| {
        let mut at = 0;
        for (first, count, id) in partitions {
            let start = first * SECTOR;
            if start < at {
                return Err(Error::Layout(format!(
                    "partition {id} at sector {first} overlaps the one before it"
                )));
            }
            zeros(sink, start - at)?;
            let made = dmg.decompress_partition_to(id, sink)?;
            if made != count * SECTOR {
                return Err(Error::Layout(format!(
                    "partition {id} made {made} bytes of {count} sectors"
                )));
            }
            at = start + made;
        }
        if at > total {
            return Err(Error::Layout(format!(
                "partitions end at byte {at}, past the image's {total}"
            )));
        }
        zeros(sink, total - at)
    })?;
    Ok(written)
}

/// The trailer's SectorCount, the disk's length. `udif` 0.4.2 reads the trailer with a 64-byte Reserved1 where Apple's
/// UDIFResourceFile has 120, so its own `sector_count` (and checksum type, checksum and image variant) is another field.
fn sector_count<R: Read + Seek>(image: &mut R) -> io::Result<u64> {
    image.seek(SeekFrom::End(SECTOR_COUNT_AT - SECTOR as i64))?;
    let mut field = [0u8; 8];
    image.read_exact(&mut field)?;
    Ok(u64::from_be_bytes(field))
}

/// The data fork's checksum covers the whole fork, which `udif` would read into memory to check. The member's own
/// SHA-256 has already been checked, and the container's content is checked by the caller.
fn open_udif<R: Read + Seek>(image: R) -> Result<DmgReader<R>> {
    Ok(DmgReader::with_options(
        image,
        DmgReaderOptions {
            verify_checksums: false,
        },
    )?)
}

fn apfs_partition<R: Read + Seek>(dmg: &DmgReader<R>) -> Result<i32> {
    let mut apfs = dmg.partitions().iter().filter(|p| p.name.contains("Apple_APFS"));
    match (apfs.next(), apfs.next()) {
        (Some(partition), None) => Ok(partition.id),
        _ => Err(Error::Layout(format!(
            "not one Apple_APFS partition in {:?}",
            dmg.list_partitions()
        ))),
    }
}

fn zeros(sink: &mut Sink, mut len: u64) -> Result<()> {
    while len > 0 {
        len -= sink.fill(|spare| {
            let n = spare.len().min(len as usize);
            spare[..n].fill(0);
            Ok((n, n as u64))
        })?;
    }
    Ok(())
}
