// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use switch_catalog::{Catalog, Role};
use switch_disk::Disk;
use switch_engine::{
    CardErrorState, CardProbe, EngineEvent, EngineStep, Mode, ProbeSlot, StepOutcome, estimate_min_partition_bytes,
};
use switch_tui::{
    BuildOption, CardOption, CardStateAfterError, Mode as TuiMode, PendingJournal, ProgressEvent, RelocationEstimate,
    SizeConstraints, SlotStatus, StepDescriptor, StepStatus, format_bytes, format_gib,
};

pub fn mode_to_tui(mode: Mode) -> TuiMode {
    match mode {
        Mode::Install => TuiMode::Install,
        Mode::Partition => TuiMode::Partition,
        Mode::PartitionAndInstall => TuiMode::PartitionAndInstall,
    }
}

pub fn mode_from_tui(mode: TuiMode) -> Mode {
    match mode {
        TuiMode::Install => Mode::Install,
        TuiMode::Partition => Mode::Partition,
        TuiMode::PartitionAndInstall => Mode::PartitionAndInstall,
    }
}

pub fn catalog_to_build_options(catalog: &Catalog) -> Vec<BuildOption> {
    catalog
        .builds
        .iter()
        .map(|record| match record.plan() {
            Ok(build) => {
                let download_bytes: u64 = Role::ALL.iter().map(|&r| build.member(r).stored_size).sum::<u64>()
                    + record.assets.values().map(|a| a.size).sum::<u64>();
                let min_part = estimate_min_partition_bytes(&build);
                BuildOption {
                    id: record.build.clone(),
                    label: format!("iPadOS {} ({})", record.version, record.build),
                    product: record.product.clone(),
                    release_tag: catalog.release.clone(),
                    size_summary: format!(
                        "~{} download · min {} partition",
                        format_bytes(download_bytes),
                        format_gib(min_part)
                    ),
                    min_partition_bytes: Some(min_part),
                    unsupported_reason: None,
                }
            }
            Err(unsupported) => {
                let download_bytes: u64 = record.members.values().map(|m| m.stored_size).sum();
                BuildOption {
                    id: record.build.clone(),
                    label: format!("iPadOS {} ({})", record.version, record.build),
                    product: record.product.clone(),
                    release_tag: catalog.release.clone(),
                    size_summary: format!("~{} download", format_bytes(download_bytes)),
                    min_partition_bytes: None,
                    unsupported_reason: Some(unsupported.to_string()),
                }
            }
        })
        .collect()
}

/// Short confirmation token matching the platform's device name (`disk4`, `sdb`, `PhysicalDrive2`).
pub fn disk_confirm_token(disk: &Disk) -> String {
    let raw = disk.path.to_string_lossy();
    if let Some(rest) = raw.strip_prefix(r"\\.\").or_else(|| raw.strip_prefix(r"\\?\")) {
        return rest.to_owned();
    }
    disk.path
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_owned)
        .unwrap_or_else(|| raw.into_owned())
}

pub fn disk_to_card_option(disk: &Disk, probe: Option<&CardProbe>) -> CardOption {
    let token = disk_confirm_token(disk);
    let name = if disk.model.is_empty() {
        format!("Removable Disk ({})", format_bytes(disk.size))
    } else {
        disk.model.clone()
    };
    let refusal = disk.refusal().map(|r| r.to_string());

    if let Some(p) = probe {
        return CardOption {
            id: token.clone(),
            name,
            capacity_bytes: p.capacity_bytes,
            layout_summary: p.layout_summary.clone(),
            slot: probe_slot_to_tui(&p.slot),
            pending_journal: p.pending_journal.as_ref().map(|j| PendingJournal {
                stage: j.stage.clone(),
                detail: j.detail.clone(),
            }),
            confirm_token: token,
            confirm_case_sensitive: !cfg!(windows),
            size_constraints: p.size.as_ref().map(probe_size_to_tui),
            preserved_items: Vec::new(),
            overwritten_items: Vec::new(),
            unusable_reason: refusal.or_else(|| p.unusable_reason.clone()),
        };
    }

    let slot = infer_slot_from_disk(disk);
    let layout_summary = summarize_disk_partitions(disk);
    let size_constraints = infer_size_constraints(disk);

    CardOption {
        id: token.clone(),
        name,
        capacity_bytes: disk.size,
        layout_summary,
        slot,
        pending_journal: None,
        confirm_token: token,
        confirm_case_sensitive: !cfg!(windows),
        size_constraints,
        preserved_items: Vec::new(),
        overwritten_items: Vec::new(),
        unusable_reason: refusal,
    }
}

pub fn image_to_card_option(path: &Path, probe: &CardProbe) -> CardOption {
    let token = path.file_name().and_then(|n| n.to_str()).unwrap_or("image").to_owned();
    CardOption {
        id: path.display().to_string(),
        name: format!("Disk image ({token})"),
        capacity_bytes: probe.capacity_bytes,
        layout_summary: probe.layout_summary.clone(),
        slot: probe_slot_to_tui(&probe.slot),
        pending_journal: probe.pending_journal.as_ref().map(|j| PendingJournal {
            stage: j.stage.clone(),
            detail: j.detail.clone(),
        }),
        confirm_token: token,
        confirm_case_sensitive: true,
        size_constraints: probe.size.as_ref().map(probe_size_to_tui),
        preserved_items: Vec::new(),
        overwritten_items: Vec::new(),
        unusable_reason: probe.unusable_reason.clone(),
    }
}

pub fn step_to_tui(step: EngineStep) -> StepDescriptor {
    StepDescriptor::new(step.id, step.label)
}

pub fn engine_event_to_tui(event: EngineEvent) -> ProgressEvent {
    match event {
        EngineEvent::SetSteps(steps) => ProgressEvent::SetSteps(steps.into_iter().map(step_to_tui).collect()),
        EngineEvent::StepStarted { step_id, irreversible } => ProgressEvent::StepStarted { step_id, irreversible },
        EngineEvent::StepProgress {
            step_id,
            done_bytes,
            total_bytes,
            bytes_per_sec,
            detail,
        } => ProgressEvent::StepProgress {
            step_id,
            done_bytes,
            total_bytes,
            bytes_per_sec,
            detail,
        },
        EngineEvent::StepFinished {
            step_id,
            outcome,
            elapsed,
        } => ProgressEvent::StepFinished {
            step_id,
            status: match outcome {
                StepOutcome::Done => StepStatus::Done,
                StepOutcome::Failed => StepStatus::Failed,
                StepOutcome::Skipped => StepStatus::Skipped,
            },
            elapsed: Some(elapsed),
        },
        EngineEvent::SetIrreversible(flag) => ProgressEvent::SetIrreversible(flag),
        EngineEvent::Log(line) => ProgressEvent::Log(line),
    }
}

pub fn card_error_state_to_tui(state: CardErrorState) -> CardStateAfterError {
    match state {
        CardErrorState::Untouched => CardStateAfterError::Untouched,
        CardErrorState::Resumable(detail) => CardStateAfterError::Resumable { detail },
        CardErrorState::Partial(detail) => CardStateAfterError::Partial { detail },
    }
}

fn probe_slot_to_tui(slot: &ProbeSlot) -> SlotStatus {
    match slot {
        ProbeSlot::None => SlotStatus::None,
        ProbeSlot::ExistingIos {
            size_bytes,
            partition_index,
        } => SlotStatus::ExistingIos {
            size_bytes: *size_bytes,
            partition_index: Some(*partition_index),
        },
        ProbeSlot::ExistingSetup {
            size_bytes,
            partition_index,
        } => SlotStatus::ExistingSetup {
            size_bytes: *size_bytes,
            partition_index: Some(*partition_index),
        },
        ProbeSlot::FreshCandidate {
            name,
            size_bytes,
            partition_index,
        } => SlotStatus::FreshCandidate {
            name: name.clone(),
            size_bytes: *size_bytes,
            partition_index: Some(*partition_index),
        },
    }
}

fn probe_size_to_tui(s: &switch_engine::ProbeSize) -> SizeConstraints {
    SizeConstraints {
        min_bytes: s.min_bytes,
        max_bytes: s.max_bytes,
        default_bytes: Some(s.default_bytes),
        total_card_bytes: s.total_card_bytes,
        fat_total_bytes: s.fat_total_bytes,
        fat_used_bytes: s.fat_used_bytes,
        other_partitions_bytes: s.other_partitions_bytes,
        relocate_estimate: RelocationEstimate::Proportional,
    }
}

fn infer_slot_from_disk(disk: &Disk) -> SlotStatus {
    for p in &disk.partitions {
        if p.name == "iOS-setup" {
            return SlotStatus::ExistingSetup {
                size_bytes: p.size,
                partition_index: p.number.checked_sub(1),
            };
        }
    }
    for p in &disk.partitions {
        if p.name == "iOS"
            || p.kind.eq_ignore_ascii_case("Apple_APFS")
            || p.kind.eq_ignore_ascii_case("7C3457EF-0000-11AA-AA11-00306543ECAC")
        {
            return SlotStatus::ExistingIos {
                size_bytes: p.size,
                partition_index: p.number.checked_sub(1),
            };
        }
    }
    for p in &disk.partitions {
        if matches!(p.name.as_str(), "userdata" | "l4t" | "UDA") {
            return SlotStatus::FreshCandidate {
                name: p.name.clone(),
                size_bytes: p.size,
                partition_index: p.number.checked_sub(1),
            };
        }
    }
    SlotStatus::None
}

fn summarize_disk_partitions(disk: &Disk) -> String {
    if disk.partitions.is_empty() {
        return format!("{} raw media (no partitions)", format_bytes(disk.size));
    }
    let parts: Vec<String> = disk
        .partitions
        .iter()
        .map(|p| {
            let label = if !p.name.is_empty() {
                p.name.as_str()
            } else if !p.kind.is_empty() {
                p.kind.as_str()
            } else {
                "partition"
            };
            format!("{} {label}", format_bytes(p.size))
        })
        .collect();
    parts.join(" · ")
}

fn infer_size_constraints(disk: &Disk) -> Option<SizeConstraints> {
    let first = disk.partitions.first()?;
    if first.size == 0 {
        return None;
    }
    let other: u64 = disk.partitions.iter().skip(1).map(|p| p.size).sum();
    let min = switch_partition::minimum_ios_bytes(0);
    let max = switch_tui::align_down(first.size.saturating_sub(64 * 1024 * 1024), switch_tui::ALIGN_BYTES);
    Some(SizeConstraints {
        min_bytes: min,
        max_bytes: max,
        default_bytes: Some(switch_tui::DEFAULT_IOS_BYTES.max(min).min(max)),
        total_card_bytes: disk.size,
        fat_total_bytes: first.size,
        fat_used_bytes: 0,
        other_partitions_bytes: other,
        relocate_estimate: RelocationEstimate::Proportional,
    })
}
