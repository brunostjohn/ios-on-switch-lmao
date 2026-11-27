// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use switch_card::{Card, Device, Role, SECTOR};
use switch_partition::journal::{self, FsKind, Phase, State};

use crate::error::Result;
use crate::target::OpenedTarget;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProbeSlot {
    None,
    ExistingIos {
        size_bytes: u64,
        partition_index: u32,
    },
    ExistingSetup {
        size_bytes: u64,
        partition_index: u32,
    },
    FreshCandidate {
        name: String,
        size_bytes: u64,
        partition_index: u32,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProbeJournal {
    pub stage: String,
    pub detail: String,
    pub entry_index: u32,
    pub ios_bytes: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProbeSize {
    pub min_bytes: u64,
    pub max_bytes: u64,
    pub default_bytes: u64,
    pub total_card_bytes: u64,
    pub fat_total_bytes: u64,
    pub fat_used_bytes: u64,
    pub other_partitions_bytes: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CardProbe {
    pub capacity_bytes: u64,
    pub mbr_only: bool,
    pub layout_summary: String,
    pub slot: ProbeSlot,
    pub pending_journal: Option<ProbeJournal>,
    pub size: Option<ProbeSize>,
    pub has_boot_volume: bool,
    pub unusable_reason: Option<String>,
}

pub fn probe_image(path: &Path, image_used: u64) -> Result<CardProbe> {
    let mut target = OpenedTarget::open_image_readonly(path, SECTOR as u32)?;
    probe_device(&mut target, image_used)
}

/// Inspects a device read-only. Always checks the partition journal before `Card::open` or `survey`.
pub fn probe_device<D: Device>(dev: &mut D, image_used: u64) -> Result<CardProbe> {
    let capacity_bytes = dev.sector_count().saturating_mul(u64::from(dev.sector_size()));
    if let Some(journal) = read_pending_journal(dev)? {
        return Ok(CardProbe {
            capacity_bytes,
            mbr_only: false,
            layout_summary: format!("Interrupted partition journal ({})", journal.stage),
            slot: ProbeSlot::None,
            pending_journal: Some(journal),
            size: None,
            has_boot_volume: false,
            unusable_reason: None,
        });
    }

    let (card_opt, mbr_only, card_err) = match Card::open(dev) {
        Ok(card) => (Some(card), false, None),
        Err(switch_card::Error::NoGpt) => (None, true, None),
        Err(err) => (None, false, Some(err.to_string())),
    };

    let (slot, has_boot_volume, gpt_summary) = match &card_opt {
        Some(card) => {
            let slot = pick_display_slot(card);
            let boot = card.boot_volume().is_ok();
            let summary = summarize_gpt(card);
            (slot, boot, Some(summary))
        }
        None => (ProbeSlot::None, false, None),
    };

    let (size, survey_summary, survey_err) = match switch_partition::survey(dev) {
        Ok(survey) => {
            let limits = survey.limits(image_used);
            let fat_total = survey.layout.fat_sectors.saturating_mul(SECTOR as u64);
            let other = other_bytes_from_card(card_opt.as_ref(), fat_total);
            let fat_used = fat_total.saturating_sub(limits.max);
            let fs_label = match survey.layout.fs {
                FsKind::Fat32 => "FAT32",
                FsKind::Exfat => "exFAT",
            };
            let prefix = if survey.mbr_only() {
                format!("MBR-only · {} {fs_label}", format_gib_short(fat_total))
            } else {
                format!("GPT + hybrid MBR · {} {fs_label}", format_gib_short(fat_total))
            };
            (
                Some(ProbeSize {
                    min_bytes: limits.min,
                    max_bytes: limits.max,
                    default_bytes: limits.default,
                    total_card_bytes: capacity_bytes,
                    fat_total_bytes: fat_total,
                    fat_used_bytes: fat_used,
                    other_partitions_bytes: other,
                }),
                Some(prefix),
                None,
            )
        }
        Err(err) => (None, None, Some(err.to_string())),
    };

    let layout_summary = match (gpt_summary, survey_summary) {
        (Some(gpt), _) => gpt,
        (None, Some(s)) => s,
        (None, None) => {
            if mbr_only {
                "MBR-only (unrecognized layout)".to_owned()
            } else {
                card_err
                    .clone()
                    .or(survey_err.clone())
                    .unwrap_or_else(|| "Unknown layout".to_owned())
            }
        }
    };

    let unusable_reason = if card_opt.is_none() && size.is_none() {
        card_err.or(survey_err)
    } else {
        None
    };

    Ok(CardProbe {
        capacity_bytes,
        mbr_only,
        layout_summary,
        slot,
        pending_journal: None,
        size,
        has_boot_volume,
        unusable_reason,
    })
}

pub fn read_pending_journal<D: Device>(dev: &mut D) -> Result<Option<ProbeJournal>> {
    match journal::read(dev) {
        Ok(State::Empty) => Ok(None),
        Ok(State::Pending(header, _)) => Ok(Some(journal_info(&header.plan, header.phase, true))),
        Ok(State::Finished(header)) => Ok(Some(journal_info(&header.plan, header.phase, false))),
        Err(switch_partition::Error::Card(switch_card::Error::TooSmall(_))) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

fn journal_info(plan: &journal::Plan, phase: Phase, pending_step: bool) -> ProbeJournal {
    let fs = match plan.fs {
        FsKind::Fat32 => "FAT32",
        FsKind::Exfat => "exFAT",
    };
    let stage = match (phase, pending_step) {
        (Phase::Relocate, true) => format!("Relocating {fs} tail clusters (step in progress)"),
        (Phase::Relocate, false) => format!("Relocating {fs} tail clusters"),
        (Phase::Shrink, _) => format!("Shrinking {fs} volume header"),
        (Phase::Tables, _) => "Updating GPT and hybrid MBR tables".to_owned(),
    };
    let ios_bytes = plan.entry_sectors.saturating_mul(SECTOR as u64);
    let detail = format!(
        "Carving {} iOS-setup partition at GPT index {} from {fs} tail",
        format_gib_short(ios_bytes),
        plan.entry_index,
    );
    ProbeJournal {
        stage,
        detail,
        entry_index: plan.entry_index,
        ios_bytes,
    }
}

fn pick_display_slot(card: &Card) -> ProbeSlot {
    let targets = card.targets();
    for role in [Role::Setup, Role::Root, Role::Fresh] {
        if let Some(t) = targets.iter().find(|t| t.role == role) {
            let size_bytes = t.sectors.saturating_mul(SECTOR as u64);
            return match role {
                Role::Setup => ProbeSlot::ExistingSetup {
                    size_bytes,
                    partition_index: t.index,
                },
                Role::Root => ProbeSlot::ExistingIos {
                    size_bytes,
                    partition_index: t.index,
                },
                Role::Fresh => {
                    let name = card
                        .entries()
                        .get(t.index as usize)
                        .map_or_else(|| "userdata".to_owned(), |e| e.name());
                    ProbeSlot::FreshCandidate {
                        name,
                        size_bytes,
                        partition_index: t.index,
                    }
                }
            };
        }
    }
    ProbeSlot::None
}

fn summarize_gpt(card: &Card) -> String {
    let mut parts = vec!["GPT + hybrid MBR".to_owned()];
    for (idx, e) in card.entries().iter().enumerate() {
        if !e.is_used() {
            continue;
        }
        let bytes = e.sectors().saturating_mul(SECTOR as u64);
        let name = e.name();
        let label = if name.is_empty() { format!("part{idx}") } else { name };
        parts.push(format!("{} {label}", format_gib_short(bytes)));
    }
    parts.join(" · ")
}

fn other_bytes_from_card(card: Option<&Card>, fat_total: u64) -> u64 {
    match card {
        Some(card) => card
            .entries()
            .iter()
            .enumerate()
            .filter(|(i, e)| *i != 0 && e.is_used())
            .map(|(_, e)| e.sectors().saturating_mul(SECTOR as u64))
            .sum(),
        None => {
            let _ = fat_total;
            0
        }
    }
}

fn format_gib_short(bytes: u64) -> String {
    const GIB: u64 = 1024 * 1024 * 1024;
    const MIB: u64 = 1024 * 1024;
    if bytes >= GIB {
        if bytes.is_multiple_of(GIB) {
            format!("{} GiB", bytes / GIB)
        } else {
            format!("{:.1} GiB", bytes as f64 / GIB as f64)
        }
    } else {
        format!("{} MiB", bytes / MIB)
    }
}
