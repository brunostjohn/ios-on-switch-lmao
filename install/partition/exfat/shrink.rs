// SPDX-License-Identifier: GPL-3.0-or-later
//! exFAT volume shrink step: backup boot region first, then main boot region, then allocation bitmap `DataLength`,
//! then terminating and freeing any surplus bitmap clusters.

use std::collections::HashMap;

use switch_card::{Device, SECTOR};

use super::boot::{
    self, BACKUP_OFFSET, CHECKSUM_SECTOR, CLUSTER_COUNT, MIN_VOLUME_SECTORS, PERCENT_IN_USE, VOLUME_FLAGS,
    VOLUME_LENGTH,
};
use super::relocate::entry_loc;
use super::{EOC, Exfat};
use crate::error::{Error, Result};
use crate::journal::{self, FieldKind, Record, Step};

pub fn step<D: Device>(dev: &mut D, fs: &Exfat, fat_new_sectors: u64, ios_bytes: u64) -> Result<Step> {
    let new_vol_len = fs.vbr.shrunk_total(fat_new_sectors);
    let new_clusters = fs.vbr.clusters_in(new_vol_len);
    if new_vol_len < MIN_VOLUME_SECTORS || new_clusters == 0 {
        return Err(Error::Size {
            bytes: ios_bytes,
            why: "the exFAT volume would be smaller than the 1 MiB minimum".into(),
        });
    }
    let cut = (new_clusters + 2) as u32;
    if fs.system_end() > cut {
        return Err(Error::Volume(
            "the allocation bitmap, up-case table, or root directory lies past the cut".into(),
        ));
    }
    if new_vol_len == fs.vbr.volume_length {
        return Ok(Step::default());
    }

    let first = fs.first;
    let mut region = vec![0u8; CHECKSUM_SECTOR as usize * SECTOR];
    dev.read_sectors(first, &mut region)?;
    let mut new_boot: [u8; SECTOR] = region[..SECTOR].try_into().unwrap();
    new_boot[VOLUME_LENGTH..VOLUME_LENGTH + 8].copy_from_slice(&new_vol_len.to_le_bytes());
    new_boot[CLUSTER_COUNT..CLUSTER_COUNT + 4].copy_from_slice(&(new_clusters as u32).to_le_bytes());
    let flags = u16::from_le_bytes([new_boot[VOLUME_FLAGS], new_boot[VOLUME_FLAGS + 1]]) & !0x0008;
    new_boot[VOLUME_FLAGS..VOLUME_FLAGS + 2].copy_from_slice(&flags.to_le_bytes());
    new_boot[PERCENT_IN_USE] = 0xff;
    region[..SECTOR].copy_from_slice(&new_boot);
    let new_cksum = boot::checksum_sector(boot::region_checksum(&region));

    let sector_rec = |dev: &mut D, lba: u64, new: [u8; SECTOR]| -> Result<Record> {
        let mut old = [0u8; SECTOR];
        dev.read_sectors(lba, &mut old)?;
        Ok(Record::Sector {
            lba,
            old: crc32fast::hash(&old),
            new: Box::new(new),
        })
    };

    let mut groups = vec![
        vec![
            sector_rec(dev, first + BACKUP_OFFSET, new_boot)?,
            sector_rec(dev, first + BACKUP_OFFSET + CHECKSUM_SECTOR, new_cksum)?,
        ],
        vec![
            sector_rec(dev, first, new_boot)?,
            sector_rec(dev, first + CHECKSUM_SECTOR, new_cksum)?,
        ],
    ];

    let new_bm_len = new_clusters.div_ceil(8);
    if new_bm_len < fs.bitmap_data_len {
        let geo = fs.geometry();
        let root_chain = fs.chain(fs.vbr.root)?;
        let (bm_lba, bm_off) = entry_loc(&geo, &root_chain, &HashMap::new(), fs.bitmap_entry_offset);
        groups.push(vec![Record::Field {
            lba: bm_lba,
            offset: bm_off,
            kind: FieldKind::ExfatBitmapLen,
            old: fs.bitmap_data_len,
            new: new_bm_len,
        }]);
        let old_bm_clusters = fs.bitmap_chain.len() as u32;
        let new_bm_clusters = new_bm_len.div_ceil(fs.vbr.cluster_bytes()) as u32;
        if new_bm_clusters < old_bm_clusters {
            let last_kept = fs.bitmap_chain[new_bm_clusters as usize - 1];
            let first_surplus = fs.bitmap_chain[new_bm_clusters as usize];
            let surplus = old_bm_clusters - new_bm_clusters;
            groups.push(vec![Record::Fat {
                index: last_kept,
                old: first_surplus,
                new: EOC,
            }]);
            groups.push(vec![
                Record::Bits {
                    first: first_surplus,
                    len: surplus,
                    set: false,
                },
                Record::Free {
                    first: first_surplus,
                    len: surplus,
                    last: EOC,
                },
            ]);
        }
    }

    let step = Step { groups };
    debug_assert!(journal::fits(&step));
    Ok(step)
}
