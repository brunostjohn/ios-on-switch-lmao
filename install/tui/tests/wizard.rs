// SPDX-License-Identifier: GPL-3.0-or-later
use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use switch_tui::{
    ALIGN_BYTES, Action, App, CardOption, DEFAULT_IOS_BYTES, Effect, Mode, RelocationEstimate, Screen, Selection,
    SizeConstraints, WizardConfig, WizardOutcome, parse_gib_to_bytes,
};

use crate::{GIB, MIB, card_with_pending_journal, sample_builds, sample_cards, sample_config};

fn type_str(app: &mut App, text: &str) {
    for ch in text.chars() {
        app.handle_action(Action::Char(ch));
    }
}

#[test]
fn wizard_install_mode_forward_and_back() {
    let mut app = App::new(sample_config(None));
    assert_eq!(app.screen(), Screen::PickMode);

    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.mode(), Mode::Install);
    assert_eq!(app.screen(), Screen::PickBuild);

    app.handle_action(Action::Down);
    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.screen(), Screen::PickCard);
    assert_eq!(app.selected_build().unwrap().id, "21H461");

    app.handle_action(Action::Back);
    assert_eq!(app.screen(), Screen::PickBuild);
    assert_eq!(app.build_index(), 1);

    app.handle_action(Action::Back);
    assert_eq!(app.screen(), Screen::PickMode);

    app.handle_action(Action::Confirm);
    app.handle_action(Action::Confirm);
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::Confirm);

    type_str(&mut app, "disk4");
    let eff = app.handle_action(Action::Confirm);
    assert_eq!(
        eff,
        Effect::WizardConfirmed(Selection {
            mode: Mode::Install,
            build_id: Some("21H461".to_owned()),
            card_id: "disk4".to_owned(),
            ios_size_bytes: None,
            resume_journal: false,
        })
    );
}

#[test]
fn wizard_partition_mode_skips_build_and_navigates_back() {
    let mut app = App::new(sample_config(None));
    app.handle_action(Action::Down);
    app.handle_action(Action::Confirm);
    assert_eq!(app.mode(), Mode::Partition);
    assert_eq!(app.screen(), Screen::PickCard);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickSize);
    assert_eq!(app.size_state().unwrap().selected_bytes, DEFAULT_IOS_BYTES);

    app.handle_action(Action::Right);
    assert_eq!(app.size_state().unwrap().selected_bytes, 65 * GIB);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::Confirm);

    app.handle_action(Action::Back);
    assert_eq!(app.screen(), Screen::PickSize);
    assert_eq!(app.size_state().unwrap().selected_bytes, 65 * GIB);

    app.handle_action(Action::Confirm);
    type_str(&mut app, "disk4");
    let eff = app.handle_action(Action::Confirm);
    assert_eq!(
        eff,
        Effect::WizardConfirmed(Selection {
            mode: Mode::Partition,
            build_id: None,
            card_id: "disk4".to_owned(),
            ios_size_bytes: Some(65 * GIB),
            resume_journal: false,
        })
    );
}

#[test]
fn wizard_partition_and_install_full_flow() {
    let mut app = App::new(sample_config(None));
    app.handle_action(Action::SelectIndex(2));
    assert_eq!(app.mode(), Mode::PartitionAndInstall);
    assert_eq!(app.screen(), Screen::PickBuild);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickCard);
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickSize);
    app.handle_action(Action::PageUp);
    assert_eq!(app.size_state().unwrap().selected_bytes, 72 * GIB);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::Confirm);
    type_str(&mut app, "disk4");
    assert_eq!(
        app.handle_action(Action::Confirm),
        Effect::WizardConfirmed(Selection {
            mode: Mode::PartitionAndInstall,
            build_id: Some("24B119".to_owned()),
            card_id: "disk4".to_owned(),
            ios_size_bytes: Some(72 * GIB),
            resume_journal: false,
        })
    );
}

#[test]
fn preselected_mode_skips_pick_mode_and_back_stops_at_first_screen() {
    let mut install_app = App::new(sample_config(Some(Mode::Install)));
    assert_eq!(install_app.screen(), Screen::PickBuild);
    install_app.handle_action(Action::Back);
    assert_eq!(install_app.screen(), Screen::PickBuild);

    let mut part_app = App::new(sample_config(Some(Mode::Partition)));
    assert_eq!(part_app.screen(), Screen::PickCard);
    part_app.handle_action(Action::Back);
    assert_eq!(part_app.screen(), Screen::PickCard);
}

#[test]
fn unsupported_build_cannot_be_selected() {
    let mut builds = sample_builds();
    builds.rotate_right(1);
    let mut app = App::new(WizardConfig {
        initial_mode: Some(Mode::Install),
        builds,
        cards: sample_cards(),
        ..WizardConfig::default()
    });
    assert_eq!(app.build_index(), 1);

    app.handle_action(Action::Up);
    assert_eq!(app.build_index(), 0);
    assert!(!app.selected_build().unwrap().is_supported());

    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.screen(), Screen::PickBuild);
    assert!(app.notice().unwrap().contains("unsupported"));
}

#[test]
fn card_refresh_and_empty_card_guard() {
    let mut app = App::new(WizardConfig {
        initial_mode: Some(Mode::Partition),
        builds: Vec::new(),
        cards: Vec::new(),
        ..WizardConfig::default()
    });
    assert_eq!(app.screen(), Screen::PickCard);
    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.screen(), Screen::PickCard);

    assert_eq!(app.handle_action(Action::RefreshCards), Effect::RefreshCards);
    app.set_cards(sample_cards());
    assert_eq!(app.cards().len(), 2);
    assert_eq!(app.card_index(), 0);

    app.handle_action(Action::Down);
    assert_eq!(app.selected_card().unwrap().id, r"\\.\PhysicalDrive2");

    let mut reversed = sample_cards();
    reversed.reverse();
    app.set_cards(reversed);
    assert_eq!(app.card_index(), 0);
    assert_eq!(app.selected_card().unwrap().id, r"\\.\PhysicalDrive2");
}

#[test]
fn pending_journal_opens_resume_prompt_before_size_or_confirm() {
    let mut cards = sample_cards();
    cards.push(card_with_pending_journal());
    let mut app = App::new(WizardConfig {
        initial_mode: Some(Mode::PartitionAndInstall),
        builds: sample_builds(),
        cards,
        ..WizardConfig::default()
    });
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickCard);

    app.handle_action(Action::Down);
    app.handle_action(Action::Down);
    assert_eq!(app.selected_card().unwrap().id, "/dev/sdb");

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::ResumeJournal);

    app.handle_action(Action::Down);
    assert_eq!(app.resume_state().selected, 1);
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickCard);

    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::ResumeJournal);
    let eff = app.handle_action(Action::Confirm);
    assert_eq!(
        eff,
        Effect::WizardConfirmed(Selection {
            mode: Mode::PartitionAndInstall,
            build_id: Some("24B119".to_owned()),
            card_id: "/dev/sdb".to_owned(),
            ios_size_bytes: None,
            resume_journal: true,
        })
    );
}

#[test]
fn size_picker_clamps_default_steps_snaps_and_parses_numeric_input() {
    let mut app = App::new(sample_config(Some(Mode::Partition)));
    app.handle_action(Action::Down);
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickSize);

    assert_eq!(app.size_state().unwrap().selected_bytes, 48 * GIB);

    app.handle_action(Action::FineDecrease);
    assert_eq!(app.size_state().unwrap().selected_bytes, 48 * GIB - 16 * MIB);
    assert_eq!(app.size_state().unwrap().selected_bytes % ALIGN_BYTES, 0);

    app.handle_action(Action::FineIncrease);
    assert_eq!(app.size_state().unwrap().selected_bytes, 48 * GIB);

    app.handle_action(Action::Right);
    assert_eq!(app.size_state().unwrap().selected_bytes, 48 * GIB);

    app.handle_action(Action::PageDown);
    assert_eq!(app.size_state().unwrap().selected_bytes, 40 * GIB);

    app.handle_action(Action::Home);
    assert_eq!(app.size_state().unwrap().selected_bytes, 16 * GIB);
    app.handle_action(Action::Left);
    assert_eq!(app.size_state().unwrap().selected_bytes, 16 * GIB);

    type_str(&mut app, "25.1");
    let snapped = app.size_state().unwrap().selected_bytes;
    assert_eq!(snapped % ALIGN_BYTES, 0);
    assert!((25 * GIB..=26 * GIB).contains(&snapped));
    assert!(
        app.size_state()
            .unwrap()
            .input_note
            .as_deref()
            .unwrap()
            .contains("16 MiB")
    );

    app.handle_action(Action::ClearInput);
    assert_eq!(app.size_state().unwrap().selected_bytes, 16 * GIB);

    type_str(&mut app, "9999");
    assert_eq!(app.size_state().unwrap().selected_bytes, 48 * GIB);
    assert!(
        app.size_state()
            .unwrap()
            .input_note
            .as_deref()
            .unwrap()
            .contains("maximum")
    );

    app.handle_action(Action::ClearInput);
    type_str(&mut app, "4");
    assert_eq!(app.size_state().unwrap().selected_bytes, 16 * GIB);
    assert!(
        app.size_state()
            .unwrap()
            .input_note
            .as_deref()
            .unwrap()
            .contains("minimum")
    );

    app.handle_action(Action::Backspace);
    assert!(app.size_state().unwrap().input_buf.is_empty());
    assert_eq!(app.size_state().unwrap().selected_bytes, 16 * GIB);

    assert_eq!(parse_gib_to_bytes("32.5"), Some(32 * GIB + 512 * MIB));
    assert_eq!(parse_gib_to_bytes("."), None);
    assert_eq!(parse_gib_to_bytes("12.3.4"), None);
}

#[test]
fn size_picker_blocks_confirm_when_card_has_insufficient_space() {
    let mut card: CardOption = sample_cards().remove(0);
    card.size_constraints = Some(SizeConstraints {
        min_bytes: 32 * GIB,
        max_bytes: 16 * GIB,
        default_bytes: None,
        total_card_bytes: 64 * GIB,
        fat_total_bytes: 60 * GIB,
        fat_used_bytes: 44 * GIB,
        other_partitions_bytes: 0,
        relocate_estimate: RelocationEstimate::Exact(0),
    });
    let mut app = App::new(WizardConfig {
        initial_mode: Some(Mode::Partition),
        builds: Vec::new(),
        cards: vec![card],
        ..WizardConfig::default()
    });
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::PickSize);
    assert!(app.size_state().unwrap().impossible);

    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.screen(), Screen::PickSize);
}

#[test]
fn confirm_screen_enforces_exact_or_normalized_disk_token() {
    let mut app = App::new(sample_config(Some(Mode::Install)));
    app.handle_action(Action::Confirm);
    app.handle_action(Action::Confirm);
    assert_eq!(app.screen(), Screen::Confirm);

    type_str(&mut app, "DISK4");
    assert_eq!(app.handle_action(Action::Confirm), Effect::None);
    assert_eq!(app.screen(), Screen::Confirm);
    assert!(app.confirm_state().error.is_some());

    let q_key = KeyEvent::new_with_kind(KeyCode::Char('q'), KeyModifiers::NONE, KeyEventKind::Press);
    assert_eq!(app.handle_key(q_key), Effect::None);
    assert_eq!(app.confirm_state().typed, "DISK4q");

    app.handle_action(Action::ClearInput);
    type_str(&mut app, "disk4");
    assert!(matches!(
        app.handle_action(Action::Confirm),
        Effect::WizardConfirmed(Selection { ref card_id, .. }) if card_id == "disk4"
    ));

    let mut win_app = App::new(sample_config(Some(Mode::Install)));
    win_app.handle_action(Action::Confirm);
    win_app.handle_action(Action::Down);
    win_app.handle_action(Action::Confirm);
    assert_eq!(win_app.screen(), Screen::Confirm);

    type_str(&mut win_app, r"\\.\physicaldrive2");
    assert!(matches!(
        win_app.handle_action(Action::Confirm),
        Effect::WizardConfirmed(Selection { ref card_id, .. }) if card_id == r"\\.\PhysicalDrive2"
    ));

    let mut cancel_app = App::new(sample_config(None));
    assert_eq!(cancel_app.handle_action(Action::Quit), Effect::WizardCancelled);
    assert_eq!(cancel_app.outcome(), Some(&WizardOutcome::Cancelled));
}

#[test]
fn key_mapping_ignores_release_events_and_handles_ctrl_c() {
    let release_enter = KeyEvent::new_with_kind(KeyCode::Enter, KeyModifiers::NONE, KeyEventKind::Release);
    assert_eq!(switch_tui::map_key(Screen::PickMode, release_enter), None);

    let ctrl_c = KeyEvent::new_with_kind(KeyCode::Char('c'), KeyModifiers::CONTROL, KeyEventKind::Press);
    assert_eq!(switch_tui::map_key(Screen::Confirm, ctrl_c), Some(Action::Quit));
    assert_eq!(switch_tui::map_key(Screen::Progress, ctrl_c), Some(Action::Quit));

    let r_key = KeyEvent::new_with_kind(KeyCode::Char('r'), KeyModifiers::NONE, KeyEventKind::Press);
    assert_eq!(switch_tui::map_key(Screen::PickCard, r_key), Some(Action::RefreshCards));
    assert_eq!(switch_tui::map_key(Screen::PickBuild, r_key), None);
}

#[test]
fn relocation_estimates_and_confirm_token_fallback() {
    let mut constraints = crate::sample_constraints();
    assert_eq!(constraints.estimate_relocation(20 * GIB), 512 * MIB);
    assert_eq!(constraints.estimate_relocation(200 * GIB), 40 * GIB);
    let mid = constraints.estimate_relocation(132 * GIB);
    assert_eq!(mid, 22 * GIB);

    constraints.relocate_estimate = RelocationEstimate::Exact(12345);
    assert_eq!(constraints.estimate_relocation(64 * GIB), 12345);

    constraints.relocate_estimate = RelocationEstimate::Proportional;
    let prop = constraints.estimate_relocation(410 * GIB);
    assert_eq!(prop, 190 * GIB);

    let mut card = sample_cards().remove(0);
    card.confirm_token.clear();
    assert_eq!(card.effective_confirm_token(), "disk4");
    assert!(card.matches_confirm("disk4"));
    assert!(!card.matches_confirm("disk5"));
}
