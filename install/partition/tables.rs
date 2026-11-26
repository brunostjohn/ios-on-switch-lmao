// SPDX-License-Identifier: GPL-3.0-or-later
//! The tables step, once the volume is shrunk: index 0 and MBR slot 0 cut to the volume, the new entry appended as
//! `iOS-setup` (Linux-typed, so never a root before `switch_card` finishes an install into it). Each sector is its
//! own group, backup copy before primary and the MBR last, as `switch_card` orders its writes.

use switch_card::{BASIC_DATA_TYPE, Card, Device, EMUMMC_TYPE, ENTRY_BYTES, Entry, LINUX_TYPE, SECTOR, SETUP_NAME};

use crate::error::{Error, Result};
use crate::gpt_new;
use crate::journal::{Plan, Record, Step};
use crate::layout::derived_guid;
use crate::mbr::{self, EMUMMC, GPT_RESERVED, LINUX};

pub fn step<D: Device>(dev: &mut D, plan: &Plan) -> Result<Step> {
    let new_entry = gpt_new::entry(
        LINUX_TYPE,
        plan.entry_guid,
        SETUP_NAME,
        plan.entry_first,
        plan.entry_sectors,
    );
    let mut groups = Vec::new();
    let writes = match plan.new_gpt {
        None => appended(dev, plan, new_entry)?,
        Some(disk_guid) => {
            let (zero, writes) = fresh(dev, plan, disk_guid, new_entry)?;
            if let Some((lba, count)) = zero {
                groups.push(vec![Record::Zero { lba, count }]);
            }
            writes
        }
    };
    for (lba, new) in writes {
        let mut old = [0u8; SECTOR];
        dev.read_sectors(lba, &mut old)?;
        groups.push(vec![Record::Sector {
            lba,
            old: crc32fast::hash(&old),
            new: Box::new(new),
        }]);
    }
    Ok(Step { groups })
}

fn appended<D: Device>(dev: &mut D, plan: &Plan, new_entry: Entry) -> Result<Vec<(u64, [u8; SECTOR])>> {
    let card = Card::open(dev)?;
    let valid_slot = plan.entry_index <= card.entries().len() as u32
        && card
            .entries()
            .get(plan.entry_index as usize)
            .is_none_or(|e| !e.is_used());
    if !valid_slot || card.boot_volume()? != (plan.fat_first, plan.fat_sectors) {
        return Err(Error::WrongCard("the GPT is not the one the run started on"));
    }
    let mut fat = card.entries()[0];
    fat.0[40..48].copy_from_slice(&(plan.fat_first + plan.fat_new_sectors - 1).to_le_bytes());
    let count = card.entries().len().max(plan.entry_index as usize + 1) as u32;
    let mut writes = card.table_writes(dev, &[(0, fat), (plan.entry_index, new_entry)], count)?;
    writes.push((0, mbr::with_length(&card.mbr(), 0, plan.fat_new_sectors)));
    Ok(writes)
}

type SectorWrite = (u64, [u8; SECTOR]);
type FreshWrites = (Option<(u64, u32)>, Vec<SectorWrite>);

fn fresh<D: Device>(dev: &mut D, plan: &Plan, disk_guid: [u8; 16], new_entry: Entry) -> Result<FreshWrites> {
    let mut sector = [0u8; SECTOR];
    dev.read_sectors(0, &mut sector)?;
    let parts = mbr::parts(&sector, plan.card_sectors)?;
    if parts[0].first != plan.fat_first || parts[0].sectors != plan.fat_sectors {
        return Err(Error::WrongCard("the MBR is not the one the run started on"));
    }
    let mut entries = Vec::new();
    let mut emummc = 0;
    for (i, part) in parts.iter().enumerate() {
        let unique = derived_guid(&format!("entry {i}"), &disk_guid);
        let (type_guid, name, sectors) = match part.kind {
            _ if i == 0 => (BASIC_DATA_TYPE, "hos_data".to_string(), plan.fat_new_sectors),
            LINUX => (LINUX_TYPE, "l4t".to_string(), part.sectors),
            EMUMMC => {
                emummc += 1;
                let name = if emummc == 1 {
                    "emummc".to_string()
                } else {
                    format!("emummc{emummc}")
                };
                (EMUMMC_TYPE, name, part.sectors)
            }
            _ => unreachable!("mbr::parts admits only these"),
        };
        entries.push(gpt_new::entry(type_guid, unique, &name, part.first, sectors));
    }
    entries.push(new_entry);
    let array_sectors = (entries.len() * ENTRY_BYTES).div_ceil(SECTOR) as u64;
    let unused_first = plan.card_sectors - (GPT_RESERVED - 1) + array_sectors;
    let unused_end = plan.card_sectors - 1;
    let zero = (unused_first < unused_end).then_some((unused_first, (unused_end - unused_first) as u32));
    let mut writes = gpt_new::sectors(plan.card_sectors, disk_guid, &entries);
    writes.push((0, mbr::hybrid(&sector, plan.fat_new_sectors, plan.card_sectors)?));
    Ok((zero, writes))
}
