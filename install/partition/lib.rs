// SPDX-License-Identifier: GPL-3.0-or-later
//! Partition mode: the iOS entry carved from the tail of the card's FAT, the files there moved below it first, every
//! step journaled at LBA 32752-32767 so a cut at any write resumes to the same end.
//!
//! ```text
//! switch_partition::resume(&mut dev)?;                   // first, always: finishes a run a cut left
//! let survey = switch_partition::survey(&mut dev)?;
//! let limits = survey.limits(image_used);                // the size picker's min, default and max
//! let plan = survey.plan(ios_bytes, limits.min)?;
//! switch_partition::run(&mut dev, &plan)?;               // entry plan.entry_index is iOS-setup: install into it
//! ```

mod apply;
mod cache;
mod copy;
mod engine;
mod error;
mod fat32;
mod gpt_new;
pub mod journal;
mod layout;
mod mbr;
mod model;
pub mod size;
mod tables;

use switch_card::{Device, SECTOR};

pub use engine::{Report, pending, resume, run};
pub use error::{Error, Result};
pub use journal::{Fingerprint, FsKind, Plan};
pub use size::{DEFAULT_IOS_BYTES, Limits, minimum_ios_bytes};

use fat32::boot::MIN_CLUSTERS;
use fat32::{Fat32, walk};
use layout::{Layout, Tables, table_crc};
use model::Tree;
use size::{ALIGN_BYTES, ALIGN_SECTORS, align_down, align_up};

/// The card read through once: its tables, its FAT and every file on it.
pub struct Survey {
    pub layout: Layout,
    fs: Fat32,
    tree: Tree,
    table_crc: u32,
}

pub fn survey<D: Device>(dev: &mut D) -> Result<Survey> {
    if journal::read(dev)? != journal::State::Empty {
        return Err(Error::Journal("a run was cut short on this card: resume it first"));
    }
    let layout = Layout::read(dev)?;
    if layout.fs != FsKind::Fat32 {
        return Err(Error::Volume("exFAT shrinking is not built yet".into()));
    }
    let fs = Fat32::load(dev, layout.fat_first, layout.fat_sectors)?;
    let tree = walk::walk(dev, &fs)?;
    if tree.lost_end > 2 {
        return Err(Error::Damaged("the volume has lost clusters".into()));
    }
    let mbr_only = matches!(layout.tables, Tables::MbrOnly(..));
    let crc = table_crc(dev, layout.capacity, mbr_only)?;
    Ok(Survey {
        layout,
        fs,
        tree,
        table_crc: crc,
    })
}

impl Survey {
    pub fn mbr_only(&self) -> bool {
        matches!(self.layout.tables, Tables::MbrOnly(..))
    }

    /// The size picker's bounds for an image whose container uses `image_used` bytes.
    pub fn limits(&self, image_used: u64) -> Limits {
        let l = &self.layout;
        let keep = u64::from(self.tree.lowest_cut(&self.fs) - 2).max(MIN_CLUSTERS);
        let fat_end = align_up(
            l.fat_first + self.fs.bpb.data_start() + keep * self.fs.bpb.cluster_sectors,
            ALIGN_SECTORS,
        );
        let entry_end = align_down(
            (l.fat_first + l.fat_sectors)
                .min(l.entry_end)
                .min(u64::from(u32::MAX) + 1),
            ALIGN_SECTORS,
        );
        Limits::new(image_used, entry_end.saturating_sub(fat_end) * SECTOR as u64)
    }

    /// The run that gives iOS `ios_bytes` (a multiple of 16 MiB, at least `min_bytes`).
    pub fn plan(&self, ios_bytes: u64, min_bytes: u64) -> Result<Plan> {
        if ios_bytes < min_bytes {
            return Err(Error::Size {
                bytes: ios_bytes,
                why: format!("below the {min_bytes} bytes this image needs"),
            });
        }
        if ios_bytes == 0 || !ios_bytes.is_multiple_of(ALIGN_BYTES) {
            return Err(Error::Size {
                bytes: ios_bytes,
                why: "not a whole number of 16 MiB".into(),
            });
        }
        let l = &self.layout;
        let ios_sectors = ios_bytes / SECTOR as u64;
        let (fat_new_sectors, entry_first) = l.carve(ios_sectors)?;
        if self.fs.bpb.clusters_in(self.fs.bpb.shrunk_total(fat_new_sectors)) < MIN_CLUSTERS {
            return Err(Error::Size {
                bytes: ios_bytes,
                why: "the FAT would keep fewer than 65,526 clusters".into(),
            });
        }
        let cut = self.fs.cut(fat_new_sectors);
        if self.tree.lost_end > 2 {
            return Err(Error::Damaged("the volume has lost clusters".into()));
        }
        let need = self.tree.owned_from(cut, self.fs.cluster_end());
        let have = (2..cut).filter(|&c| self.fs.fat[c as usize] == 0).count() as u64;
        if need > have {
            return Err(Error::NoRoom { need, have });
        }
        Ok(Plan {
            card_sectors: l.capacity,
            fat_first: l.fat_first,
            fat_sectors: l.fat_sectors,
            fat_new_sectors,
            entry_first,
            entry_sectors: ios_sectors,
            entry_index: l.entry_index,
            entry_guid: l.guid("ios entry"),
            fs: l.fs,
            new_gpt: self.mbr_only().then(|| l.guid("disk")),
            fingerprint: Fingerprint {
                vol_serial: self.fs.bpb.serial,
                vol_boot_sig: self.fs.bpb.boot_sig,
                vol_total: self.fs.bpb.total,
                boot_crc: self.fs.bpb.boot_crc,
                table_crc: self.table_crc,
            },
        })
    }
}

impl Tree {
    fn lowest_cut(&self, fs: &Fat32) -> u32 {
        model::lowest_cut(
            fs.cluster_end(),
            |c| self.owned.get(c),
            |c| fs.fat[c as usize] == 0,
            self.lost_end,
        )
    }
}
