// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;
use std::sync::{Arc, Mutex, mpsc};
use std::thread;
use std::time::Duration;

use switch_catalog::Catalog;
use switch_engine::{
    CancelToken as EngineCancel, EngineEvent, EngineFailure, Mode, ProgressSink, RunRequest, RunSummary, StepOutcome,
    run_detailed, steps_for_mode,
};
use switch_tui::{
    CancelToken as TuiCancel, DoneSummary, ErrorSummary, FinalStatus, ProgressEvent, Selection, WizardConfig,
    format_bytes, run_terminal,
};

use crate::cli::Cli;
use crate::convert::{
    card_error_state_to_tui, catalog_to_build_options, engine_event_to_tui, mode_from_tui, mode_to_tui, step_to_tui,
};
use crate::discover::{DiscoveredCard, discover_cards, discover_image_card};

pub fn run_tui_session(
    cli: &Cli,
    catalog: Option<Catalog>,
    initial_cards: Vec<DiscoveredCard>,
) -> Result<bool, String> {
    let builds = catalog.as_ref().map(catalog_to_build_options).unwrap_or_default();
    let card_options = initial_cards.iter().map(|c| c.option.clone()).collect();
    let shared_cards = Arc::new(Mutex::new(initial_cards));
    let config = WizardConfig {
        title: "iPadOS Switch Installer".to_owned(),
        initial_mode: cli.mode.map(Into::into),
        builds,
        cards: card_options,
    };

    let eject = !cli.no_eject;
    let force_unmount = cli.force_unmount;
    let image_override = cli.image.clone();
    let cli_clone = cli.clone();
    let refresh_store = Arc::clone(&shared_cards);
    let worker_store = Arc::clone(&shared_cards);

    let status = run_terminal(
        config,
        move || {
            let refreshed = match &image_override {
                Some(path) => vec![discover_image_card(path, 0)?],
                None => discover_cards(eject, force_unmount)?,
            };
            let options = refreshed.iter().map(|c| c.option.clone()).collect();
            if let Ok(mut guard) = refresh_store.lock() {
                *guard = refreshed;
            }
            Ok(options)
        },
        move |selection, tui_cancel| {
            let known_cards = worker_store.lock().map(|g| g.clone()).unwrap_or_default();
            spawn_tui_worker(cli_clone, catalog, known_cards, selection, tui_cancel)
        },
    )
    .map_err(|e| e.to_string())?;

    match status {
        FinalStatus::CancelledInWizard => {
            println!("Cancelled.");
            Ok(true)
        }
        FinalStatus::Done(summary) => {
            println!("{}", summary.title);
            if let Some(log) = summary.log_path {
                println!("Log: {log}");
            }
            Ok(true)
        }
        FinalStatus::Error(err) => {
            eprintln!("{}: {}", err.title, err.message);
            if let Some(log) = err.log_path {
                eprintln!("Log: {log}");
            }
            Ok(false)
        }
    }
}

fn spawn_tui_worker(
    cli: Cli,
    catalog: Option<Catalog>,
    cards: Vec<DiscoveredCard>,
    selection: Selection,
    tui_cancel: TuiCancel,
) -> (Vec<switch_tui::StepDescriptor>, mpsc::Receiver<ProgressEvent>) {
    let mode = mode_from_tui(selection.mode);
    let pretranslate = catalog
        .as_ref()
        .and_then(|c| selection.build_id.as_deref().and_then(|id| c.record(id)))
        .and_then(|r| r.plan().ok())
        .is_some_and(|b| b.steps.pretranslate);
    let steps: Vec<_> = steps_for_mode(mode, pretranslate)
        .into_iter()
        .map(step_to_tui)
        .collect();
    let (tx, rx) = mpsc::channel();

    thread::spawn(move || {
        let Some(card) = cards.into_iter().find(|c| c.option.id == selection.card_id) else {
            let _ = tx.send(ProgressEvent::Failed(ErrorSummary {
                title: "Card not found".to_owned(),
                message: format!("Selected card {:?} is no longer available.", selection.card_id),
                log_path: None,
                card_state: switch_tui::CardStateAfterError::Untouched,
                failed_step: None,
            }));
            return;
        };

        let engine_cancel = EngineCancel::new();
        let watcher_engine = engine_cancel.clone();
        let watcher_tui = tui_cancel.clone();
        let done_flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let done_watch = std::sync::Arc::clone(&done_flag);
        thread::spawn(move || {
            while !done_watch.load(std::sync::atomic::Ordering::Relaxed) {
                if watcher_tui.is_cancelled() {
                    watcher_engine.cancel();
                    break;
                }
                thread::sleep(Duration::from_millis(25));
            }
        });

        let mut request = RunRequest::new(mode, card.target);
        request.build_id = selection.build_id.or(cli.build.clone());
        request.partition_index = cli.partition;
        request.ios_size_bytes = selection.ios_size_bytes.or(cli.ios_size);
        request.resume_journal = selection.resume_journal || cli.resume;
        request.work_dir = cli.cache_dir.clone();
        request.log_path = cli.log_file.clone();
        request.local = cli.local_inputs();

        let tx_sink = tx.clone();
        let mut sink = move |ev: EngineEvent| {
            let _ = tx_sink.send(engine_event_to_tui(ev));
        };

        let res = run_detailed(&request, catalog.as_ref(), &mut sink, engine_cancel);
        done_flag.store(true, std::sync::atomic::Ordering::Relaxed);

        match res {
            Ok(summary) => {
                let _ = tx.send(ProgressEvent::Finished(done_summary_from_engine(&summary)));
            }
            Err(failure) => {
                if matches!(*failure.error, switch_engine::EngineError::Cancelled) {
                    let _ = tx.send(ProgressEvent::Cancelled {
                        message: "Operation cancelled before writing to the microSD card.".to_owned(),
                        log_path: failure.log_path.map(|p| p.display().to_string()),
                    });
                } else {
                    let _ = tx.send(ProgressEvent::Failed(error_summary_from_engine(&failure)));
                }
            }
        }
    });

    (steps, rx)
}

pub fn run_cli_session(request: &RunRequest, catalog: Option<&Catalog>) -> Result<RunSummary, EngineFailure> {
    let mut sink = PlainCliSink;
    let cancel = EngineCancel::new();
    run_detailed(request, catalog, &mut sink, cancel)
}

struct PlainCliSink;

impl ProgressSink for PlainCliSink {
    fn emit(&mut self, event: EngineEvent) {
        match event {
            EngineEvent::SetSteps(_) => {}
            EngineEvent::StepStarted { step_id, irreversible } => {
                if irreversible {
                    eprintln!("==> {step_id} (writing to target)");
                } else {
                    eprintln!("==> {step_id}");
                }
            }
            EngineEvent::StepProgress {
                step_id,
                done_bytes,
                total_bytes,
                bytes_per_sec,
                detail,
            } => {
                let detail = detail.unwrap_or_default();
                match (total_bytes, bytes_per_sec) {
                    (Some(total), Some(rate)) if total > 0 => {
                        let pct = (done_bytes as f64 / total as f64 * 100.0).min(100.0);
                        eprintln!(
                            "    [{step_id}] {pct:5.1}% ({} / {}, {}/s) {detail}",
                            format_bytes(done_bytes),
                            format_bytes(total),
                            format_bytes(rate as u64)
                        );
                    }
                    _ if !detail.is_empty() => eprintln!("    [{step_id}] {detail}"),
                    _ => {}
                }
            }
            EngineEvent::StepFinished {
                step_id,
                outcome,
                elapsed,
            } => {
                let tag = match outcome {
                    StepOutcome::Done => "done",
                    StepOutcome::Failed => "FAILED",
                    StepOutcome::Skipped => "skipped",
                };
                eprintln!("<== {step_id}: {tag} ({:.2?})", elapsed);
            }
            EngineEvent::SetIrreversible(true) => {
                eprintln!("*** Card writes started (irreversible)");
            }
            EngineEvent::SetIrreversible(false) => {}
            EngineEvent::Log(line) => eprintln!("    {line}"),
        }
    }
}

fn done_summary_from_engine(summary: &RunSummary) -> DoneSummary {
    let title = match summary.mode {
        Mode::Partition => {
            if summary.resumed_journal {
                "Interrupted partition resumed and completed".to_owned()
            } else {
                "microSD card partitioning complete".to_owned()
            }
        }
        Mode::Install => "iPadOS installation complete".to_owned(),
        Mode::PartitionAndInstall => "Partition and iPadOS installation complete".to_owned(),
    };
    let mut details = Vec::new();
    if let Some(build) = &summary.build {
        details.push(format!("Installed build: {build}"));
    }
    if let (Some(idx), Some(bytes)) = (summary.partition_index, summary.partition_bytes) {
        details.push(format!("GPT partition {idx}: {}", format_bytes(bytes)));
    }
    if summary.boot_files_written > 0 {
        details.push(format!("Updated {} FAT boot file(s)", summary.boot_files_written));
    }
    if summary.resumed_journal {
        details.push("Resumed previously interrupted partition journal cleanly.".to_owned());
    }
    details.push("Preserved Horizon OS, emuMMC, and switchroot/ios/bootargs.txt.".to_owned());

    DoneSummary {
        mode: mode_to_tui(summary.mode),
        title,
        card_label: summary.card_label.clone(),
        ejected: summary.ejected,
        details,
        next_steps: Vec::new(),
        log_path: path_to_opt_string(&summary.log_path),
    }
}

fn error_summary_from_engine(failure: &EngineFailure) -> ErrorSummary {
    ErrorSummary {
        title: "Operation failed".to_owned(),
        message: failure.error.to_string(),
        log_path: path_to_opt_string(&failure.log_path),
        card_state: card_error_state_to_tui(failure.card_state()),
        failed_step: failure.failed_step.clone(),
    }
}

fn path_to_opt_string(path: &Option<PathBuf>) -> Option<String> {
    path.as_ref().map(|p| p.display().to_string())
}
