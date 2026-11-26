// SPDX-License-Identifier: GPL-3.0-or-later
use std::sync::mpsc;
use std::time::Duration;

use switch_tui::{
    Action, App, CardStateAfterError, DoneSummary, Effect, ErrorSummary, MAX_LOG_LINES, Mode, ProgressEvent, Screen,
    StepDescriptor, StepStatus,
};

use crate::MIB;

fn progress_app() -> App {
    App::new_progress(
        "iPadOS Switch Installer",
        Mode::Install,
        vec![
            StepDescriptor::new("download", "Download IPSW"),
            StepDescriptor::new("write", "Write to card"),
            StepDescriptor::new("boot", "Write boot entry"),
        ],
    )
}

#[test]
fn progress_updates_steps_rate_eta_and_done_screen() {
    let mut app = progress_app();
    assert_eq!(app.screen(), Screen::Progress);

    app.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "download".to_owned(),
        irreversible: false,
    });
    app.handle_action(Action::Tick(Duration::from_secs(2)));
    assert_eq!(app.progress_state().steps[0].elapsed, Duration::from_secs(2));

    app.apply_progress_event(ProgressEvent::StepProgress {
        step_id: "download".to_owned(),
        done_bytes: 100 * MIB,
        total_bytes: Some(400 * MIB),
        bytes_per_sec: None,
        detail: Some("Downloading OS image".to_owned()),
    });
    let active = app.progress_state().active.as_ref().unwrap();
    let bps = active.bytes_per_sec.unwrap();
    assert!((bps - (50.0 * MIB as f64)).abs() < 1024.0);
    assert_eq!(active.eta, Some(Duration::from_secs(6)));

    app.apply_progress_event(ProgressEvent::StepFinished {
        step_id: "download".to_owned(),
        status: StepStatus::Done,
        elapsed: Some(Duration::from_secs(8)),
    });
    assert_eq!(app.progress_state().steps[0].status, StepStatus::Done);
    assert_eq!(app.progress_state().steps[0].elapsed, Duration::from_secs(8));

    app.apply_progress_event(ProgressEvent::StepFinished {
        step_id: "boot".to_owned(),
        status: StepStatus::Skipped,
        elapsed: None,
    });
    assert_eq!(app.progress_state().steps[2].status, StepStatus::Skipped);

    app.apply_progress_event(ProgressEvent::Finished(DoneSummary {
        mode: Mode::Install,
        title: "iPadOS 27.2 installed".to_owned(),
        card_label: "disk4 — SanDisk 1TB".to_owned(),
        ejected: true,
        details: vec!["Completed in 02:14".to_owned()],
        next_steps: Vec::new(),
        log_path: Some("/tmp/install.log".to_owned()),
    }));
    assert_eq!(app.screen(), Screen::Done);
    assert_eq!(app.done_summary().unwrap().effective_next_steps().len(), 3);
    assert_eq!(app.handle_action(Action::Confirm), Effect::Exit);
}

#[test]
fn log_tailing_pauses_scrolls_and_bounds_memory() {
    let mut app = progress_app();
    for i in 0..25 {
        app.apply_progress_event(ProgressEvent::Log(format!("line {i}")));
    }
    assert!(app.progress_state().follow_logs);
    assert_eq!(app.progress_state().visible_log_slice(5), (20, 25));

    app.handle_action(Action::Up);
    assert!(!app.progress_state().follow_logs);
    assert_eq!(app.progress_state().visible_log_slice(5), (19, 24));

    app.apply_progress_event(ProgressEvent::Log("line 25".to_owned()));
    assert_eq!(app.progress_state().visible_log_slice(5), (19, 24));

    app.handle_action(Action::End);
    assert!(app.progress_state().follow_logs);
    assert_eq!(app.progress_state().visible_log_slice(5), (21, 26));

    for i in 0..(MAX_LOG_LINES + 50) {
        app.apply_progress_event(ProgressEvent::Log(format!("burst {i}")));
    }
    assert_eq!(app.progress_state().logs.len(), MAX_LOG_LINES);
}

#[test]
fn cancel_allowed_before_irreversible_and_blocked_during_card_writes() {
    let mut app = progress_app();
    app.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "download".to_owned(),
        irreversible: false,
    });
    assert_eq!(app.handle_action(Action::Quit), Effect::RequestCancel);
    assert!(app.progress_state().cancel_requested);

    app.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "write".to_owned(),
        irreversible: true,
    });
    assert_eq!(app.handle_action(Action::Quit), Effect::None);
    assert!(app.progress_state().irreversible_warning);
    assert_eq!(app.screen(), Screen::Progress);
}

#[test]
fn error_cancel_and_disconnected_worker_transitions() {
    let mut app = progress_app();
    app.apply_progress_event(ProgressEvent::Cancelled {
        message: "Cancelled by user".to_owned(),
        log_path: Some("/tmp/cancel.log".to_owned()),
    });
    assert_eq!(app.screen(), Screen::Error);
    assert_eq!(app.error_summary().unwrap().card_state, CardStateAfterError::Untouched);

    let mut failed = progress_app();
    failed.apply_progress_event(ProgressEvent::Failed(ErrorSummary {
        title: "Partition failed".to_owned(),
        message: "I/O timeout".to_owned(),
        log_path: Some("/tmp/fail.log".to_owned()),
        card_state: CardStateAfterError::Resumable {
            detail: "Journal saved at LBA 32752".to_owned(),
        },
        failed_step: Some("Relocate FAT".to_owned()),
    }));
    assert_eq!(failed.screen(), Screen::Error);
    assert_eq!(failed.handle_action(Action::Quit), Effect::Exit);

    let (tx, rx) = mpsc::channel();
    let mut disconnected = progress_app();
    disconnected.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "write".to_owned(),
        irreversible: true,
    });
    drop(tx);
    assert!(disconnected.drain_progress(&rx));
    assert_eq!(disconnected.screen(), Screen::Error);
    assert!(matches!(
        disconnected.error_summary().unwrap().card_state,
        CardStateAfterError::Partial { .. }
    ));
}

#[test]
fn cancel_token_multiline_logs_and_irreversible_toggle() {
    let token = switch_tui::CancelToken::new();
    let cloned = token.clone();
    assert!(!token.is_cancelled());
    cloned.cancel();
    assert!(token.is_cancelled());

    let mut app = progress_app();
    app.apply_progress_event(ProgressEvent::SetSteps(vec![StepDescriptor::new("only", "Only step")]));
    assert_eq!(app.progress_state().steps.len(), 1);

    app.apply_progress_event(ProgressEvent::Log("first\nsecond\r\nthird".to_owned()));
    assert_eq!(app.progress_state().logs.len(), 3);
    assert_eq!(app.progress_state().logs[1], "second");

    app.apply_progress_event(ProgressEvent::SetIrreversible(true));
    assert_eq!(app.handle_action(Action::Quit), Effect::None);
    assert!(app.progress_state().irreversible_warning);
    app.apply_progress_event(ProgressEvent::SetIrreversible(false));
    assert!(!app.progress_state().irreversible_warning);
    assert_eq!(app.handle_action(Action::Quit), Effect::RequestCancel);
}
