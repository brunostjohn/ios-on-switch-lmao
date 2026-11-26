// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::FileSource;
use switch_apfs_read::{Container, Fs};
use switch_apfs_write::target::stale_descriptor_area;
use switch_apfs_write::{CheckpointAreas, NewVolume, SourcePins, WriteOptions};
use switch_card::{Card, SECTOR};
use switch_catalog::{Build, Catalog};
use switch_image::parse_uuid;
use switch_sd::BootFile;

use crate::assemble::{AssembleInput, assemble_volumes};
use crate::boot::prepare_boot;
use crate::error::{EngineError, Result};
use crate::fetch::{prepare_members, resolve_release_assets};
use crate::pretranslate::prepare_pretranslated;
use crate::probe::read_pending_journal;
use crate::progress::{EngineProgress, EngineStep, StepOutcome};
use crate::resume::{ResumeState, ResumeStore, partition_fingerprint};
use crate::target::{OpenedTarget, ProgressBlockTarget};
use crate::{Mode, RunRequest, RunSummary, estimate_image_used_bytes};

pub fn steps_for_mode(mode: Mode, pretranslate: bool) -> Vec<EngineStep> {
    let mut steps = Vec::new();
    if matches!(mode, Mode::Partition | Mode::PartitionAndInstall) {
        steps.push(EngineStep::new("partition", "Partition microSD card"));
    }
    if matches!(mode, Mode::Install | Mode::PartitionAndInstall) {
        steps.push(EngineStep::new("prepare", "Fetch and decode release & IPSW"));
        if pretranslate {
            steps.push(EngineStep::new("pretranslate", "Pretranslate arm64e binaries"));
        }
        steps.push(EngineStep::new("assemble", "Build boot files & plan APFS volumes"));
        steps.push(EngineStep::new("write_apfs", "Stream APFS container to partition"));
        steps.push(EngineStep::new("write_boot", "Write FAT boot files & finalize GPT"));
    }
    steps
}

pub fn run_partition(request: &RunRequest, image_used: u64, progress: &mut EngineProgress<'_>) -> Result<RunSummary> {
    progress.set_steps(steps_for_mode(Mode::Partition, false));
    progress.check_cancel()?;
    let mut target = OpenedTarget::open(&request.target)?;
    let part = execute_partition_stage(&mut target, request, image_used, progress)?;
    let ejected = target.finish()?;
    Ok(RunSummary {
        mode: Mode::Partition,
        card_label: request.target.label(),
        build: None,
        ejected,
        resumed_journal: part.resumed,
        partition_index: Some(part.entry_index),
        partition_bytes: Some(part.entry_bytes),
        boot_files_written: 0,
        log_path: progress.log_path(),
    })
}

pub fn run_install(
    request: &RunRequest,
    catalog: &Catalog,
    build: &Build,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<RunSummary> {
    progress.set_steps(steps_for_mode(Mode::Install, build.steps.pretranslate));
    progress.check_cancel()?;
    let mut target = OpenedTarget::open(&request.target)?;

    let resumed = check_install_journal(&mut target, request.resume_journal, progress)?;
    let outcome = execute_install_stage(&mut target, request, catalog, build, work_dir, progress)?;
    let ejected = target.finish()?;

    Ok(RunSummary {
        mode: Mode::Install,
        card_label: request.target.label(),
        build: Some(build.build().to_owned()),
        ejected,
        resumed_journal: resumed,
        partition_index: Some(outcome.partition_index),
        partition_bytes: Some(outcome.partition_bytes),
        boot_files_written: outcome.boot_files_written,
        log_path: progress.log_path(),
    })
}

pub fn run_partition_and_install(
    request: &RunRequest,
    catalog: &Catalog,
    build: &Build,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<RunSummary> {
    progress.set_steps(steps_for_mode(Mode::PartitionAndInstall, build.steps.pretranslate));
    progress.check_cancel()?;

    let image_used = request
        .image_used_override
        .unwrap_or_else(|| estimate_image_used_bytes(build));
    let mut target = OpenedTarget::open(&request.target)?;
    let part = execute_partition_stage(&mut target, request, image_used, progress)?;

    let mut install_req = request.clone();
    install_req.partition_index = Some(part.entry_index);
    let outcome = execute_install_stage(&mut target, &install_req, catalog, build, work_dir, progress)?;
    let ejected = target.finish()?;

    Ok(RunSummary {
        mode: Mode::PartitionAndInstall,
        card_label: request.target.label(),
        build: Some(build.build().to_owned()),
        ejected,
        resumed_journal: part.resumed,
        partition_index: Some(outcome.partition_index),
        partition_bytes: Some(outcome.partition_bytes),
        boot_files_written: outcome.boot_files_written,
        log_path: progress.log_path(),
    })
}

struct PartitionOutcome {
    entry_index: u32,
    entry_bytes: u64,
    resumed: bool,
}

fn execute_partition_stage(
    target: &mut OpenedTarget,
    request: &RunRequest,
    image_used: u64,
    progress: &mut EngineProgress<'_>,
) -> Result<PartitionOutcome> {
    progress.check_cancel()?;

    if let Some(pending) = read_pending_journal(target)? {
        progress.start_step("partition", true)?;
        progress.log(format!("Resuming interrupted partition: {}", pending.stage));
        let report = switch_partition::resume(target)?
            .ok_or_else(|| EngineError::Config("partition journal disappeared before resume".into()))?;
        progress.log(format!(
            "Partition resumed ({} bytes moved, {} batches)",
            report.moved_bytes, report.batches
        ));
        progress.finish_step("partition", StepOutcome::Done);
        return Ok(PartitionOutcome {
            entry_index: pending.entry_index,
            entry_bytes: pending.ios_bytes,
            resumed: true,
        });
    }

    let survey = switch_partition::survey(target)?;
    let limits = survey.limits(image_used);
    if !limits.possible() {
        return Err(EngineError::Config(format!(
            "not enough free space on boot volume: need at least {} bytes, at most {} bytes available",
            limits.min, limits.max
        )));
    }
    let ios_bytes = request.ios_size_bytes.unwrap_or(limits.default);
    let plan = survey.plan(ios_bytes, limits.min)?;

    progress.start_step("partition", true)?;
    progress.log(format!(
        "Carving {} iOS-setup partition at GPT index {} (LBA {}..={})",
        ios_bytes,
        plan.entry_index,
        plan.entry_first,
        plan.entry_last()
    ));
    let report = switch_partition::run(target, &plan)?;
    progress.log(format!(
        "Partition complete ({} bytes relocated in {} batches)",
        report.moved_bytes, report.batches
    ));
    progress.finish_step("partition", StepOutcome::Done);
    Ok(PartitionOutcome {
        entry_index: plan.entry_index,
        entry_bytes: plan.entry_sectors.saturating_mul(SECTOR as u64),
        resumed: false,
    })
}

/// Checks `switch_partition::pending` BEFORE calling `Card::open` so a torn partition table is resumed first.
fn check_install_journal(
    target: &mut OpenedTarget,
    resume_journal: bool,
    progress: &mut EngineProgress<'_>,
) -> Result<bool> {
    let Some(pending) = read_pending_journal(target)? else {
        return Ok(false);
    };
    if !resume_journal {
        return Err(EngineError::PendingJournal {
            stage: pending.stage,
            detail: pending.detail,
        });
    }
    progress.check_cancel()?;
    progress.set_irreversible(true);
    progress.log(format!(
        "Resuming interrupted partition before install: {}",
        pending.stage
    ));
    let report = switch_partition::resume(target)?;
    if let Some(r) = report {
        progress.log(format!(
            "Interrupted partition finished ({} bytes moved)",
            r.moved_bytes
        ));
    }
    Ok(true)
}

struct InstallOutcome {
    partition_index: u32,
    partition_bytes: u64,
    boot_files_written: usize,
}

fn execute_install_stage(
    target: &mut OpenedTarget,
    request: &RunRequest,
    catalog: &Catalog,
    build: &Build,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<InstallOutcome> {
    let mut card = Card::open(target)?;
    let (fat_first, fat_sectors) = card.boot_volume()?;
    let min_need = build.container(switch_catalog::Role::Os).map_or(0, |(_, p)| p.size);
    let card_plan = card.plan(request.partition_index, min_need)?;
    let target_entry = card.entries()[card_plan.target.index as usize];
    let fingerprint = partition_fingerprint(
        card.sector_count(),
        card_plan.target.index,
        card_plan.target.first,
        card_plan.target.sectors,
        &target_entry.unique_guid(),
    );

    progress.start_step("prepare", false)?;
    let assets = resolve_release_assets(catalog, build, &request.local, work_dir, progress)?;
    let need_im4p = request.local.boot_files_dir.is_none()
        || (build.steps.pretranslate
            && (request.local.pretranslated_os_dir.is_none()
                || request.local.pretranslated_app_cryptex_dir.is_none()
                || request.local.pretranslated_system_cryptex_dir.is_none()));
    let members = prepare_members(build, &request.local, work_dir, need_im4p, progress)?;
    progress.finish_step("prepare", StepOutcome::Done);

    let deltas = if build.steps.pretranslate {
        progress.start_step("pretranslate", false)?;
        let res = prepare_pretranslated(
            build,
            &request.local,
            assets.boot_tar.as_deref(),
            &members,
            work_dir,
            progress,
        )?;
        progress.finish_step("pretranslate", StepOutcome::Done);
        res
    } else {
        None
    };

    progress.start_step("assemble", false)?;
    let os_src =
        FileSource::open(&members.os_container, 0).map_err(|e| EngineError::io_path(&members.os_container, e))?;
    let os_container = Container::open(&os_src)?;
    let system_uuid = parse_uuid(&build.record.volumes.os_volume_uuid)?;
    let system_vol = os_container
        .volume(system_uuid)
        .ok_or_else(|| EngineError::Config("OS container lacks pinned System volume UUID".into()))?;
    let system_fs = Fs::open(&os_src, system_vol)?;

    let boot = prepare_boot(
        build,
        &request.local,
        assets.boot_tar.as_deref(),
        members.im4p.as_ref(),
        &system_fs,
        progress,
    )?;
    let assembled = assemble_volumes(
        &AssembleInput {
            build,
            local: &request.local,
            assets: &assets,
            members: &members,
            system_fs: &system_fs,
            boot: &boot,
            deltas: deltas.as_ref(),
            time_ns: request.time_ns,
        },
        progress,
    )?;

    let resume_store = ResumeStore::new(work_dir);
    let partition_win = target.partition_window(card_plan.target.first, card_plan.target.sectors)?;
    let mut resume_state = match resume_store.load(&fingerprint, card_plan.target.first, card_plan.target.sectors)? {
        Some(mut existing) if existing.phase_b_started => {
            existing.reconcile_build(build.build());
            progress.log(format!(
                "Loaded persisted stale descriptor areas ({}) for interrupted write resume",
                existing.stale_descriptor_areas.len()
            ));
            existing
        }
        _ => {
            // Read once before the first phase B because phase B zeroes block 0 first.
            let stale = stale_descriptor_area(&partition_win)?;
            ResumeState::new(
                fingerprint.clone(),
                build.build().to_owned(),
                card_plan.target.first,
                card_plan.target.sectors,
                &stale,
            )
        }
    };

    let target_blocks = partition_win.blocks();
    let stale_ranges = resume_state.stale_ranges();
    let new_volumes = [
        NewVolume {
            spec: &assembled.data_spec,
            tree: &assembled.data_tree,
        },
        NewVolume {
            spec: &assembled.preboot_spec,
            tree: &assembled.preboot_tree,
        },
    ];
    let write_opts = WriteOptions {
        target_blocks,
        areas: CheckpointAreas::Rule,
        time_ns: request.time_ns,
        formatted_by: "switch_engine".to_owned(),
    };
    let apfs_plan = switch_apfs_write::plan(
        &os_src,
        &SourcePins { system_uuid },
        &new_volumes,
        &write_opts,
        &stale_ranges,
    )?;
    let planned_bytes = (apfs_plan.summary.copied_blocks
        + apfs_plan.summary.relocated_blocks
        + apfs_plan.summary.new_data_blocks
        + apfs_plan.metadata.len() as u64)
        .saturating_mul(BLOCK_SIZE as u64);
    progress.log(format!(
        "APFS plan ready: {} target blocks, {} free blocks (~{} bytes to write)",
        apfs_plan.target_blocks, apfs_plan.summary.free_blocks, planned_bytes
    ));
    progress.finish_step("assemble", StepOutcome::Done);

    progress.start_step("write_apfs", true)?;
    card.mark(target, &card_plan)?;
    resume_state.phase_b_started = true;
    resume_store.save(&resume_state)?;

    {
        let partition_win = target.partition_window(card_plan.target.first, card_plan.target.sectors)?;
        let mut progress_target = ProgressBlockTarget::new(partition_win, progress, "write_apfs", planned_bytes);
        let stats = switch_apfs_write::write(&os_src, &mut progress_target, &apfs_plan)?;
        let total_written = progress_target.written_bytes();
        drop(progress_target);
        for st in &stats {
            progress.log(format!(
                "APFS {:?}: {} writes, {} bytes ({:.1} MB/s)",
                st.phase,
                st.writes,
                st.bytes,
                st.mb_per_s()
            ));
        }
        progress.step_progress(
            "write_apfs",
            total_written,
            Some(total_written.max(planned_bytes)),
            Some("APFS container written".to_owned()),
            true,
        );
    }
    progress.finish_step("write_apfs", StepOutcome::Done);

    progress.start_step("write_boot", true)?;
    let boot_files: Vec<BootFile<'_>> = boot
        .sd_files
        .iter()
        .map(|(path, data)| BootFile {
            path: path.as_str(),
            data: data.as_slice(),
        })
        .collect();
    let sd_summary = switch_sd::install(target, fat_first, fat_sectors, &boot_files)?;
    progress.log(format!(
        "Boot volume updated: {} written, {} unchanged, {} removed",
        sd_summary.written.len(),
        sd_summary.unchanged.len(),
        sd_summary.removed.len()
    ));

    card.finish(target, &card_plan)?;
    resume_store.clear(&fingerprint)?;
    progress.log(format!(
        "Promoted GPT partition {} to iOS (demoted {} old root(s) to userdata)",
        card_plan.target.index,
        card_plan.others.len()
    ));
    progress.finish_step("write_boot", StepOutcome::Done);

    Ok(InstallOutcome {
        partition_index: card_plan.target.index,
        partition_bytes: card_plan.target.sectors.saturating_mul(SECTOR as u64),
        boot_files_written: sd_summary.written.len(),
    })
}
