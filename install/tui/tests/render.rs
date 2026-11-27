// SPDX-License-Identifier: GPL-3.0-or-later
use std::time::Duration;

use switch_tui::{
    Action, App, CardStateAfterError, DoneSummary, ErrorSummary, Mode, ProgressEvent, Screen, StepDescriptor,
    StepStatus, WizardConfig,
};

use crate::{MIB, card_with_pending_journal, render_to_string, sample_builds, sample_cards, sample_config};

const VIEWPORTS: [(u16, u16); 3] = [(60, 18), (80, 24), (120, 40)];

fn assert_renders_all_viewports(app: &App, expected_snippets: &[&str]) {
    for &(w, h) in &VIEWPORTS {
        let out = render_to_string(app, w, h);
        assert_eq!(out.lines().count(), h as usize, "height mismatch at {w}x{h}");
        for snippet in expected_snippets {
            assert!(
                out.contains(snippet),
                "missing {snippet:?} at {w}x{h} on {:?}:\n{out}",
                app.screen()
            );
        }
    }
}

#[test]
fn render_all_wizard_screens_across_viewports() {
    let mut app = App::new(sample_config(None));
    assert_renders_all_viewports(&app, &["Install iPadOS", "Partition microSD"]);

    app.handle_action(Action::SelectIndex(2));
    assert_eq!(app.screen(), Screen::PickBuild);
    assert_renders_all_viewports(&app, &["iPadOS 27.2", "Unsupported"]);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickCard);
    assert_renders_all_viewports(&app, &["disk4", "SanDisk"]);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickSize);
    assert_renders_all_viewports(&app, &["64 GiB", "FAT32", "iOS"]);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::Confirm);
    assert_renders_all_viewports(&app, &["disk4", "bootargs.txt"]);

    app.handle_action(Action::Char('w'));
    app.handle_action(Action::Confirm);
    assert_renders_all_viewports(&app, &["Expected"]);
}

#[test]
fn render_empty_cards_and_resume_journal_across_viewports() {
    let empty_app = App::new(WizardConfig {
        initial_mode: Some(Mode::Partition),
        builds: Vec::new(),
        cards: Vec::new(),
        ..WizardConfig::default()
    });
    assert_renders_all_viewports(&empty_app, &["No removable", "'r'"]);

    let mut cards = sample_cards();
    cards.push(card_with_pending_journal());
    let mut resume_app = App::new(WizardConfig {
        initial_mode: Some(Mode::Install),
        builds: sample_builds(),
        cards,
        ..WizardConfig::default()
    });
    resume_app.handle_action(Action::Confirm);
    resume_app.handle_action(Action::Down);
    resume_app.handle_action(Action::Down);
    assert_renders_all_viewports(&resume_app, &["RESUME REQUIRED"]);

    resume_app.handle_action(Action::Confirm);
    assert_eq!(resume_app.screen(), Screen::ResumeJournal);
    assert_renders_all_viewports(&resume_app, &["Resume Interrupted Partition", "Relocating FAT32"]);
}

#[test]
fn render_progress_done_and_error_screens_across_viewports() {
    let mut app = App::new_progress(
        "iPadOS Switch Installer",
        Mode::Install,
        vec![
            StepDescriptor::new("download", "Download IPSW"),
            StepDescriptor::new("build", "Build APFS"),
            StepDescriptor::new("write", "Write microSD"),
        ],
    );
    app.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "download".to_owned(),
        irreversible: false,
    });
    app.apply_progress_event(ProgressEvent::StepProgress {
        step_id: "download".to_owned(),
        done_bytes: 50 * MIB,
        total_bytes: Some(100 * MIB),
        bytes_per_sec: Some(25.0 * MIB as f64),
        detail: None,
    });
    app.apply_progress_event(ProgressEvent::Log("Connected to release CDN".to_owned()));
    assert_renders_all_viewports(&app, &["50.0%", "MiB/s", "Download IPSW", "Connected to release CDN"]);

    app.apply_progress_event(ProgressEvent::StepFinished {
        step_id: "download".to_owned(),
        status: StepStatus::Done,
        elapsed: Some(Duration::from_secs(4)),
    });
    app.apply_progress_event(ProgressEvent::StepStarted {
        step_id: "write".to_owned(),
        irreversible: true,
    });
    app.apply_progress_event(ProgressEvent::StepProgress {
        step_id: "write".to_owned(),
        done_bytes: 12 * MIB,
        total_bytes: None,
        bytes_per_sec: None,
        detail: Some("Syncing GPT".to_owned()),
    });
    app.handle_action(Action::Tick(Duration::from_millis(50)));
    app.handle_action(Action::Quit);
    assert_renders_all_viewports(&app, &["WARNING", "Write microSD"]);

    app.apply_progress_event(ProgressEvent::Finished(DoneSummary {
        mode: Mode::Install,
        title: "iPadOS 27.2 installed".to_owned(),
        card_label: "disk4 — SanDisk 1TB".to_owned(),
        ejected: true,
        details: vec!["All checks passed".to_owned()],
        next_steps: Vec::new(),
        log_path: Some("/tmp/switch.log".to_owned()),
    }));
    assert_eq!(app.screen(), Screen::Done);
    assert_renders_all_viewports(&app, &["safely ejected", "hekate", "iOS"]);

    let mut err_app = App::new_progress(
        "iPadOS Switch Installer",
        Mode::Partition,
        vec![StepDescriptor::new("shrink", "Shrink FAT32")],
    );
    err_app.apply_progress_event(ProgressEvent::Log("sector write failed".to_owned()));
    err_app.apply_progress_event(ProgressEvent::Failed(ErrorSummary {
        title: "Partition failed".to_owned(),
        message: "Disk disconnected".to_owned(),
        log_path: Some("/tmp/err.log".to_owned()),
        card_state: CardStateAfterError::Resumable {
            detail: "Re-run to resume".to_owned(),
        },
        failed_step: Some("Shrink FAT32".to_owned()),
    }));
    assert_eq!(err_app.screen(), Screen::Error);
    assert_renders_all_viewports(&err_app, &["Partition failed", "Resumable", "/tmp/err.log"]);
}

#[test]
fn render_edge_states_and_tiny_viewports_never_panic() {
    let mut card = sample_cards().remove(0);
    card.size_constraints.as_mut().unwrap().min_bytes = 250 * crate::GIB;
    let mut impossible = App::new(WizardConfig {
        initial_mode: Some(Mode::Partition),
        builds: Vec::new(),
        cards: vec![card],
        ..WizardConfig::default()
    });
    impossible.handle_action(Action::Confirm);
    assert_renders_all_viewports(&impossible, &["Insufficient Free Space"]);

    let mut not_ejected = App::new_progress("Installer", Mode::Partition, Vec::new());
    not_ejected.apply_progress_event(ProgressEvent::Finished(DoneSummary {
        mode: Mode::Partition,
        title: "Partitioned".to_owned(),
        card_label: "disk4".to_owned(),
        ejected: false,
        details: Vec::new(),
        next_steps: Vec::new(),
        log_path: None,
    }));
    assert_renders_all_viewports(&not_ejected, &["Safely eject", "Install mode"]);

    for (w, h) in [(8, 4), (40, 12)] {
        let _ = render_to_string(&impossible, w, h);
        let _ = render_to_string(&not_ejected, w, h);
    }
}
