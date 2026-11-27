// SPDX-License-Identifier: GPL-3.0-or-later
use crate::device::Device;
use crate::entry::{APFS_TYPE, BASIC_DATA_TYPE, EMUMMC_TYPE, Entry, FRESH_NAMES, LINUX_TYPE, ROOT_NAME, SETUP_NAME};
use crate::error::Error;
use crate::gpt::{SECTOR, Tables};

const PROTECTIVE: u8 = 0xee;
const FAT_MBR_TYPES: [u8; 3] = [0x0b, 0x0c, 0x07];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// What the kernel boots (`switchSdFindRoot`).
    Root,
    /// An install that had not finished.
    Setup,
    /// A Linux partition an install may take over.
    Fresh,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub index: u32,
    pub role: Role,
    pub first: u64,
    pub sectors: u64,
}

/// The card's partition tables as read: the MBR and both GPT copies, with the primary array taken as the card's.
#[derive(Clone)]
pub struct Card {
    pub(crate) mbr: [u8; SECTOR],
    pub(crate) tables: Tables,
    pub(crate) entries: Vec<Entry>,
}

impl Card {
    pub fn open<D: Device>(dev: &mut D) -> Result<Card, Error> {
        if dev.sector_size() != SECTOR as u32 {
            return Err(Error::SectorSize(dev.sector_size()));
        }
        if dev.sector_count() < 68 {
            return Err(Error::TooSmall(dev.sector_count()));
        }
        let mut mbr = [0; SECTOR];
        dev.read_sectors(0, &mut mbr)?;
        let header = mbrman::MBRHeader::from_bytes(&mbr).map_err(|e| Error::Mbr(e.to_string()))?;
        if !header.iter().any(|(_, p)| p.sys == PROTECTIVE) {
            return Err(Error::NoGpt);
        }
        let tables = Tables::read(dev)?;
        let entries = (0..tables.count()).map(|i| tables.primary.entry(i)).collect();
        let card = Card { mbr, tables, entries };
        card.check_entries()?;
        Ok(card)
    }

    /// `parse_entry`'s rules: every used entry inside the usable range, a non-zero unique GUID, no overlaps.
    pub(crate) fn check_entries(&self) -> Result<(), Error> {
        let header = &self.tables.primary.header;
        let size = header.entry_size() as usize;
        for (i, e) in self.used() {
            let bad = |why| Err(Error::Entry { index: i, why });
            let at = self.tables.primary.entry_at(i);
            if self.tables.primary.array[at + 128..at + size].iter().any(|&b| b != 0) {
                return bad("bytes past the first 128 are not zero");
            }
            if e.unique_guid() == [0; 16] {
                return bad("zero unique GUID");
            }
            if e.first() < header.first_usable() || e.first() > e.last() || e.last() > header.last_usable() {
                return bad("extent outside the usable range");
            }
            for (j, other) in self.used().take_while(|&(j, _)| j < i) {
                if other.unique_guid() == e.unique_guid() {
                    return Err(Error::Entry {
                        index: j,
                        why: "unique GUID repeated",
                    });
                }
                if e.first() <= other.last() && other.first() <= e.last() {
                    return bad("overlaps an earlier entry");
                }
            }
        }
        Ok(())
    }

    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    /// True when the two GPT copies disagree: a write here was cut short.
    pub fn torn(&self) -> bool {
        !self.tables.clean()
    }

    pub fn sector_count(&self) -> u64 {
        self.tables.backup.header_lba + 1
    }

    fn used(&self) -> impl Iterator<Item = (u32, &Entry)> {
        self.entries
            .iter()
            .enumerate()
            .filter(|(_, e)| e.is_used())
            .map(|(i, e)| (i as u32, e))
    }

    pub fn roots(&self) -> Vec<u32> {
        self.used()
            .filter(|(_, e)| e.is_kernel_root())
            .map(|(i, _)| i)
            .collect()
    }

    pub fn role(&self, index: u32) -> Option<Role> {
        let e = self.entries.get(index as usize).filter(|e| e.is_used())?;
        if index == 0 || e.type_guid() == BASIC_DATA_TYPE || e.type_guid() == EMUMMC_TYPE {
            None
        } else if e.type_guid() == APFS_TYPE || e.named(ROOT_NAME) {
            Some(Role::Root)
        } else if e.named(SETUP_NAME) {
            Some(Role::Setup)
        } else if e.type_guid() == LINUX_TYPE && FRESH_NAMES.iter().any(|n| e.named(n)) {
            Some(Role::Fresh)
        } else {
            None
        }
    }

    /// Every partition an install could go into, the FAT (GPT index 0) and emuMMC never among them.
    pub fn targets(&self) -> Vec<Target> {
        self.used()
            .filter_map(|(i, e)| {
                self.role(i).map(|role| Target {
                    index: i,
                    role,
                    first: e.first(),
                    sectors: e.sectors(),
                })
            })
            .collect()
    }

    /// `choice`, or else the one unfinished install, else the one root, else the one fresh partition.
    pub fn target(&self, choice: Option<u32>) -> Result<Target, Error> {
        let targets = self.targets();
        if let Some(index) = choice {
            return targets
                .into_iter()
                .find(|t| t.index == index)
                .ok_or(Error::NotATarget(index));
        }
        for role in [Role::Setup, Role::Root, Role::Fresh] {
            let of_role: Vec<&Target> = targets.iter().filter(|t| t.role == role).collect();
            match of_role.as_slice() {
                [] => continue,
                [one] => return Ok(**one),
                many => return Err(Error::Ambiguous(many.iter().map(|t| t.index).collect())),
            }
        }
        Err(Error::NoTarget)
    }

    /// The target, checked for an image of `need_bytes` and addressable by the kernel. The kernel takes a root at any
    /// sector, as the owner's card shows (its root starts 1 MiB past a 16 MiB boundary); 16 MiB alignment is the rule
    /// for entries partition mode creates.
    pub fn plan(&self, choice: Option<u32>, need_bytes: u64) -> Result<Plan, Error> {
        let target = self.target(choice)?;
        let index = target.index;
        let e = &self.entries[index as usize];
        if !e.addressable() {
            return Err(Error::Unaddressable { index });
        }
        let have = e.sectors() * SECTOR as u64;
        if have < need_bytes {
            return Err(Error::TooSmallForImage {
                index,
                have,
                need: need_bytes,
            });
        }
        let others: Vec<u32> = self
            .used()
            .filter(|&(i, e)| i != index && (e.is_kernel_root() || e.named(SETUP_NAME)))
            .map(|(i, _)| i)
            .collect();
        if let Some(&foreign) = others.iter().find(|&&i| self.role(i).is_none()) {
            return Err(Error::ForeignRoot(foreign));
        }
        Ok(Plan { target, others })
    }

    /// The FAT hekate boots from: MBR slot 0, mirrored exactly by GPT index 0, a Microsoft basic data partition.
    pub fn boot_volume(&self) -> Result<(u64, u64), Error> {
        let slot = &self.mbr[446..462];
        if !FAT_MBR_TYPES.contains(&slot[4]) {
            return Err(Error::NoBootVolume("MBR slot 0 is not a FAT32 or exFAT partition"));
        }
        let first = u64::from(u32::from_le_bytes(slot[8..12].try_into().unwrap()));
        let sectors = u64::from(u32::from_le_bytes(slot[12..16].try_into().unwrap()));
        let fat = &self.entries[0];
        if fat.type_guid() != BASIC_DATA_TYPE || fat.first() != first || fat.sectors() != sectors {
            return Err(Error::NoBootVolume(
                "GPT index 0 is not the basic data partition MBR slot 0 maps",
            ));
        }
        Ok((first, sectors))
    }
}

/// What an install does to the tables: `target` becomes the root, and every other root or unfinished install goes
/// back to `userdata`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    pub target: Target,
    pub others: Vec<u32>,
}
