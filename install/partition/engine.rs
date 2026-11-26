// SPDX-License-Identifier: GPL-3.0-or-later
//! A run from start to end, and a resume from wherever a cut left one: every cluster past the cut moved below it,
//! then the volume shrunk, then the tables, then the journal cleared.

use switch_card::{Card, Device, LINUX_TYPE, SECTOR, SETUP_NAME};

use crate::apply::{Geometry, apply};
use crate::copy::Copier;
use crate::error::{Error, Result};
use crate::fat32::boot::{self, Bpb};
use crate::fat32::relocate;
use crate::fat32::{self, Fat32, walk};
use crate::journal::{self, FieldKind, FsKind, Phase, Plan, Record, State, Step};
use crate::layout::{Layout, Tables, table_crc};
use crate::tables;

#[derive(Clone, Debug, Default)]
pub struct Report {
    pub moved_bytes: u64,
    pub copy_seconds: f64,
    /// Bytes a second through the copies, barriers included.
    pub throughput: f64,
    pub batches: u64,
    pub resumed: bool,
}

struct Run<'a> {
    plan: &'a Plan,
    seq: u64,
    copier: Copier,
    batches: u64,
}

pub fn run<D: Device>(dev: &mut D, plan: &Plan) -> Result<Report> {
    if journal::read(dev)? != State::Empty {
        return Err(Error::Journal("a run was cut short on this card: resume it first"));
    }
    check_card(dev, plan)?;
    let layout = Layout::read(dev)?;
    let same = (layout.fat_first, layout.fat_sectors, layout.entry_index, layout.fs)
        == (plan.fat_first, plan.fat_sectors, plan.entry_index, plan.fs)
        && (matches!(layout.tables, Tables::MbrOnly(..)) == plan.new_gpt.is_some());
    if !same || layout.carve(plan.entry_sectors)? != (plan.fat_new_sectors, plan.entry_first) {
        return Err(Error::WrongCard("the plan was made for another card"));
    }
    verify_card(dev, plan, Phase::Relocate, None)?;
    journal::begin(dev, plan)?;
    let mut run = Run::new(plan, 1);
    run.from(dev, Phase::Relocate)?;
    Ok(run.report(false))
}

/// Finishes a run a cut left on the card; None when there is none. Call it before reading the card's tables with
/// anything else: mid-way, the GPT copies may disagree in ways `switch_card` refuses.
pub fn resume<D: Device>(dev: &mut D) -> Result<Option<Report>> {
    let (header, step) = match journal::read(dev)? {
        State::Empty => return Ok(None),
        State::Pending(header, step) => (header, Some(step)),
        State::Finished(header) => (header, None),
    };
    verify_card(dev, &header.plan, header.phase, step.as_ref())?;
    let mut seq = header.seq + 1;
    if let Some(step) = step {
        let geo = geometry(dev, &header.plan)?;
        apply(dev, &geo, &step)?;
        journal::finish(dev, seq, header.phase, &header.plan)?;
        seq += 1;
    }
    let mut run = Run::new(&header.plan, seq);
    match header.phase {
        Phase::Relocate => run.from(dev, Phase::Relocate)?,
        Phase::Shrink => run.from(dev, Phase::Tables)?,
        Phase::Tables => journal::clear(dev)?,
    }
    Ok(Some(run.report(true)))
}

/// The plan read from a journal a cut left, without touching anything.
pub fn pending<D: Device>(dev: &mut D) -> Result<Option<Plan>> {
    Ok(match journal::read(dev)? {
        State::Empty => None,
        State::Pending(h, _) | State::Finished(h) => Some(h.plan),
    })
}

fn check_card<D: Device>(dev: &mut D, plan: &Plan) -> Result<()> {
    if dev.sector_size() != SECTOR as u32 {
        return Err(switch_card::Error::SectorSize(dev.sector_size()).into());
    }
    if dev.sector_count() < journal::FIRST + journal::SECTORS {
        return Err(switch_card::Error::TooSmall(dev.sector_count()).into());
    }
    if dev.sector_count() != plan.card_sectors {
        return Err(Error::WrongCard("its size differs"));
    }
    if plan.fs != FsKind::Fat32 {
        return Err(Error::Volume("exFAT shrinking is not built yet".into()));
    }
    Ok(())
}

fn verify_card<D: Device>(dev: &mut D, plan: &Plan, phase: Phase, step: Option<&Step>) -> Result<()> {
    check_card(dev, plan)?;
    let mut boot = [0u8; SECTOR];
    dev.read_sectors(plan.fat_first, &mut boot)?;
    let fp = &plan.fingerprint;
    if boot[66] != fp.vol_boot_sig
        || u32::from_le_bytes(boot[67..71].try_into().unwrap()) != fp.vol_serial
        || boot::boot_crc(&boot) != fp.boot_crc
    {
        return Err(Error::WrongCard("the boot volume does not match the journal"));
    }
    let bpb = Bpb::parse(&boot, plan.fat_sectors)?;
    let shrunk = fp.vol_total.min(plan.fat_new_sectors);
    let total_ok = match (phase, step.is_some()) {
        (Phase::Relocate, _) => bpb.total == fp.vol_total,
        (Phase::Shrink, true) => bpb.total == fp.vol_total || bpb.total == shrunk,
        (Phase::Shrink, false) | (Phase::Tables, _) => bpb.total == shrunk,
    };
    if !total_ok {
        return Err(Error::WrongCard("the boot volume's length does not match the journal"));
    }
    let mbr_only = plan.new_gpt.is_some();
    match (phase, step) {
        (Phase::Relocate | Phase::Shrink, _) => {
            if table_crc(dev, plan.card_sectors, mbr_only)? != fp.table_crc {
                return Err(Error::WrongCard("the partition tables do not match the journal"));
            }
        }
        (Phase::Tables, Some(step)) => verify_tables_step(dev, step)?,
        (Phase::Tables, None) => verify_tables_done(dev, plan)?,
    }
    Ok(())
}

fn verify_tables_step<D: Device>(dev: &mut D, step: &Step) -> Result<()> {
    let mut sector = [0u8; SECTOR];
    for r in step.groups.iter().flatten() {
        if let Record::Sector { lba, old, new } = r {
            dev.read_sectors(*lba, &mut sector)?;
            if sector != **new && crc32fast::hash(&sector) != *old {
                return Err(Error::Changed {
                    lba: *lba,
                    what: "a partition table sector",
                });
            }
        }
    }
    Ok(())
}

fn verify_tables_done<D: Device>(dev: &mut D, plan: &Plan) -> Result<()> {
    let wrong = || Error::WrongCard("the finished partition tables do not match the journal");
    let card = Card::open(dev).map_err(|_| wrong())?;
    if card.torn() || card.boot_volume().ok() != Some((plan.fat_first, plan.fat_new_sectors)) {
        return Err(wrong());
    }
    if let Some(disk_guid) = plan.new_gpt
        && card.disk_guid() != disk_guid
    {
        return Err(wrong());
    }
    let Some(entry) = card.entries().get(plan.entry_index as usize) else {
        return Err(wrong());
    };
    if !entry.is_used()
        || !entry.named(SETUP_NAME)
        || entry.type_guid() != LINUX_TYPE
        || entry.unique_guid() != plan.entry_guid
        || entry.first() != plan.entry_first
        || entry.sectors() != plan.entry_sectors
    {
        return Err(wrong());
    }
    Ok(())
}

fn geometry<D: Device>(dev: &mut D, plan: &Plan) -> Result<Geometry> {
    Ok(fat32::geometry(plan.fat_first, &read_bpb(dev, plan)?))
}

fn read_bpb<D: Device>(dev: &mut D, plan: &Plan) -> Result<Bpb> {
    let mut sector = [0u8; SECTOR];
    dev.read_sectors(plan.fat_first, &mut sector)?;
    Bpb::parse(&sector, plan.fat_sectors)
}

impl<'a> Run<'a> {
    fn new(plan: &'a Plan, seq: u64) -> Run<'a> {
        Run {
            plan,
            seq,
            copier: Copier::default(),
            batches: 0,
        }
    }

    fn from<D: Device>(&mut self, dev: &mut D, phase: Phase) -> Result<()> {
        if phase == Phase::Relocate {
            self.relocate(dev)?;
            self.shrink(dev)?;
        }
        let step = tables::step(dev, self.plan)?;
        self.commit(dev, Phase::Tables, &step)?;
        journal::clear(dev)
    }

    fn commit<D: Device>(&mut self, dev: &mut D, phase: Phase, step: &Step) -> Result<()> {
        journal::commit(dev, self.seq, phase, self.plan, step)?;
        self.seq += 1;
        let geo = geometry(dev, self.plan)?;
        apply(dev, &geo, step)?;
        journal::finish(dev, self.seq, phase, self.plan)?;
        self.seq += 1;
        Ok(())
    }

    fn relocate<D: Device>(&mut self, dev: &mut D) -> Result<()> {
        let mut fs = Fat32::load(dev, self.plan.fat_first, self.plan.fat_sectors)?;
        let cut = fs.cut(self.plan.fat_new_sectors);
        let mut tree = walk::walk(dev, &fs)?;
        if tree.lost_end > 2 {
            return Err(Error::Damaged("the volume has lost clusters".into()));
        }
        let need = tree.owned_from(cut, fs.cluster_end());
        let have = (2..cut).filter(|&c| fs.fat[c as usize] == 0).count() as u64;
        if need > have {
            return Err(Error::NoRoom { need, have });
        }
        let affected = tree.affected(cut);
        let mut cursor = 0;
        let geo = fs.geometry();
        while let Some(batch) = relocate::plan(&fs, &tree, &affected, cut, &mut cursor)? {
            self.copier.copy(dev, &batch.copies)?;
            journal::commit(dev, self.seq, Phase::Relocate, self.plan, &batch.step)?;
            self.seq += 1;
            apply(dev, &geo, &batch.step)?;
            journal::finish(dev, self.seq, Phase::Relocate, self.plan)?;
            self.seq += 1;
            batch.apply_to(&mut fs, &mut tree);
            self.batches += 1;
        }
        if (cut..fs.cluster_end()).any(|c| fs.is_used(c)) {
            return Err(Error::Damaged("clusters past the cut are still in use".into()));
        }
        Ok(())
    }

    /// `TotSec32` in the backup boot sector, then the main one; FSInfo's free count and next free cluster unknown.
    fn shrink<D: Device>(&mut self, dev: &mut D) -> Result<()> {
        let bpb = read_bpb(dev, self.plan)?;
        let total = bpb.shrunk_total(self.plan.fat_new_sectors);
        if bpb.clusters_in(total) < boot::MIN_CLUSTERS {
            return Err(Error::Size {
                bytes: self.plan.entry_sectors * SECTOR as u64,
                why: "the volume would keep too few clusters to stay FAT32".into(),
            });
        }
        let first = self.plan.fat_first;
        let tot_sec = |lba: u64| Record::Field {
            lba,
            offset: boot::TOT_SEC32,
            kind: FieldKind::Le32,
            old: bpb.total,
            new: total,
        };
        let mut backup = Vec::new();
        let backup_fsi = (bpb.backup_boot != 0 && bpb.fs_info != 0 && bpb.backup_boot + bpb.fs_info < bpb.reserved)
            .then_some(bpb.backup_boot);
        let bases = [(bpb.fs_info != 0).then_some(0), backup_fsi];
        for base in bases.into_iter().flatten() {
            let lba = first + base + bpb.fs_info;
            let mut s = [0u8; SECTOR];
            dev.read_sectors(lba, &mut s)?;
            if !boot::is_fs_info(&s) {
                continue;
            }
            for offset in [boot::FSI_FREE_COUNT, boot::FSI_NXT_FREE] {
                let at = offset as usize;
                backup.push(Record::Field {
                    lba,
                    offset,
                    kind: FieldKind::Le32,
                    old: u64::from(u32::from_le_bytes(s[at..at + 4].try_into().unwrap())),
                    new: u64::from(u32::MAX),
                });
            }
        }
        if bpb.backup_boot != 0 {
            backup.push(tot_sec(first + bpb.backup_boot));
        }
        let step = Step {
            groups: vec![backup, vec![tot_sec(first)]],
        };
        self.commit(dev, Phase::Shrink, &step)
    }

    fn report(&self, resumed: bool) -> Report {
        Report {
            moved_bytes: self.copier.bytes,
            copy_seconds: self.copier.time.as_secs_f64(),
            throughput: self.copier.throughput(),
            batches: self.batches,
            resumed,
        }
    }
}
