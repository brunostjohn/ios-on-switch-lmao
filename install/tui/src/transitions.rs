// SPDX-License-Identifier: GPL-3.0-or-later
use crate::model::{COARSE_STEP_BYTES, FINE_STEP_BYTES, Mode, PAGE_STEP_BYTES, Selection, WizardOutcome};
use crate::size_state::SizeState;
use crate::state::{Action, App, ConfirmState, Effect, ResumeState, Screen};

impl App {
    pub(crate) fn handle_pick_mode(&mut self, action: Action) -> Effect {
        match action {
            Action::Up => {
                self.mode_index = self.mode_index.saturating_sub(1);
                self.mode = Mode::ALL[self.mode_index];
                self.notice = None;
                Effect::None
            }
            Action::Down => {
                self.mode_index = (self.mode_index + 1).min(Mode::ALL.len() - 1);
                self.mode = Mode::ALL[self.mode_index];
                self.notice = None;
                Effect::None
            }
            Action::SelectIndex(idx) if idx < Mode::ALL.len() => {
                self.mode_index = idx;
                self.mode = Mode::ALL[idx];
                self.advance_from_mode()
            }
            Action::Confirm => {
                self.mode = Mode::ALL[self.mode_index];
                self.advance_from_mode()
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    fn advance_from_mode(&mut self) -> Effect {
        self.notice = None;
        self.screen = if self.mode.needs_build() {
            Screen::PickBuild
        } else {
            Screen::PickCard
        };
        Effect::None
    }

    pub(crate) fn handle_pick_build(&mut self, action: Action) -> Effect {
        match action {
            Action::Up => {
                self.build_index = self.build_index.saturating_sub(1);
                self.notice = None;
                Effect::None
            }
            Action::Down => {
                if !self.builds.is_empty() {
                    self.build_index = (self.build_index + 1).min(self.builds.len() - 1);
                }
                self.notice = None;
                Effect::None
            }
            Action::Confirm => {
                let Some(build) = self.builds.get(self.build_index) else {
                    self.notice = Some("No builds are available in the catalog.".to_owned());
                    return Effect::None;
                };
                if let Some(reason) = &build.unsupported_reason {
                    self.notice = Some(format!("Cannot select unsupported build: {reason}"));
                    return Effect::None;
                }
                self.notice = None;
                self.screen = Screen::PickCard;
                Effect::None
            }
            Action::Back => {
                self.notice = None;
                if !self.mode_fixed {
                    self.screen = Screen::PickMode;
                }
                Effect::None
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    pub(crate) fn handle_pick_card(&mut self, action: Action) -> Effect {
        match action {
            Action::Up => {
                self.card_index = self.card_index.saturating_sub(1);
                self.notice = None;
                Effect::None
            }
            Action::Down => {
                if !self.cards.is_empty() {
                    self.card_index = (self.card_index + 1).min(self.cards.len() - 1);
                }
                self.notice = None;
                Effect::None
            }
            Action::RefreshCards => {
                self.notice = Some("Refreshing removable cards...".to_owned());
                Effect::RefreshCards
            }
            Action::Confirm => self.advance_from_card(),
            Action::Back => {
                self.notice = None;
                if self.mode.needs_build() {
                    self.screen = Screen::PickBuild;
                } else if !self.mode_fixed {
                    self.screen = Screen::PickMode;
                }
                Effect::None
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    fn advance_from_card(&mut self) -> Effect {
        let Some(card) = self.cards.get(self.card_index).cloned() else {
            self.notice = Some("No removable microSD card selected. Insert a card and press r.".to_owned());
            return Effect::None;
        };

        if card.pending_journal.is_some() {
            self.notice = None;
            self.resume = ResumeState { selected: 0 };
            self.screen = Screen::ResumeJournal;
            return Effect::None;
        }

        if let Some(reason) = &card.unusable_reason {
            self.notice = Some(format!("Card cannot be used: {reason}"));
            return Effect::None;
        }

        if self.mode.needs_size() {
            let Some(constraints) = card.size_constraints.clone() else {
                self.notice = Some("Selected card has no partitionable FAT32 layout.".to_owned());
                return Effect::None;
            };
            let build_id = self.selected_build().map(|b| b.id.clone());
            let build_min = self.selected_build().and_then(|b| b.min_partition_bytes);
            let same_context = self.size.is_some()
                && self.size_card_id.as_deref() == Some(card.id.as_str())
                && self.size_build_id == build_id;
            if !same_context {
                self.size = Some(SizeState::new(constraints, build_min));
                self.size_card_id = Some(card.id);
                self.size_build_id = build_id;
            }
            self.notice = None;
            self.screen = Screen::PickSize;
        } else {
            self.enter_confirm();
        }
        Effect::None
    }

    pub(crate) fn handle_resume(&mut self, action: Action) -> Effect {
        match action {
            Action::Up => {
                self.resume.selected = 0;
                Effect::None
            }
            Action::Down => {
                self.resume.selected = (self.resume.selected + 1) % 2;
                Effect::None
            }
            Action::SelectIndex(0) => self.confirm_resume(),
            Action::Confirm => {
                if self.resume.selected == 0 {
                    self.confirm_resume()
                } else {
                    self.screen = Screen::PickCard;
                    Effect::None
                }
            }
            Action::Back => {
                self.screen = Screen::PickCard;
                Effect::None
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    fn confirm_resume(&mut self) -> Effect {
        let Some(card) = self.selected_card() else {
            self.screen = Screen::PickCard;
            return Effect::None;
        };
        let selection = Selection {
            mode: self.mode,
            build_id: self.selected_build().map(|b| b.id.clone()),
            card_id: card.id.clone(),
            ios_size_bytes: None,
            resume_journal: true,
        };
        self.outcome = Some(WizardOutcome::Confirmed(selection.clone()));
        Effect::WizardConfirmed(selection)
    }

    pub(crate) fn handle_pick_size(&mut self, action: Action) -> Effect {
        match action {
            Action::Left => self.step_size(-(COARSE_STEP_BYTES as i64)),
            Action::Right => self.step_size(COARSE_STEP_BYTES as i64),
            Action::PageDown => self.step_size(-(PAGE_STEP_BYTES as i64)),
            Action::PageUp => self.step_size(PAGE_STEP_BYTES as i64),
            Action::FineDecrease => self.step_size(-(FINE_STEP_BYTES as i64)),
            Action::FineIncrease => self.step_size(FINE_STEP_BYTES as i64),
            Action::Home => {
                if let Some(s) = &mut self.size {
                    s.set_to_min();
                }
                self.notice = None;
                Effect::None
            }
            Action::End => {
                if let Some(s) = &mut self.size {
                    s.set_to_max();
                }
                self.notice = None;
                Effect::None
            }
            Action::Char(c) => {
                if let Some(s) = &mut self.size {
                    s.push_input_char(c);
                }
                self.notice = None;
                Effect::None
            }
            Action::Backspace => {
                if let Some(s) = &mut self.size {
                    s.pop_input_char();
                }
                Effect::None
            }
            Action::ClearInput => {
                if let Some(s) = &mut self.size {
                    s.clear_input();
                }
                Effect::None
            }
            Action::Confirm => {
                if self.size.as_ref().is_none_or(|s| s.impossible) {
                    self.notice = Some("Not enough free space on FAT32 for the minimum iOS partition.".to_owned());
                    return Effect::None;
                }
                self.enter_confirm();
                Effect::None
            }
            Action::Back => {
                self.notice = None;
                self.screen = Screen::PickCard;
                Effect::None
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    fn step_size(&mut self, delta: i64) -> Effect {
        if let Some(s) = &mut self.size {
            s.step_by(delta);
        }
        self.notice = None;
        Effect::None
    }

    fn enter_confirm(&mut self) {
        self.confirm = ConfirmState::default();
        self.notice = None;
        self.screen = Screen::Confirm;
    }

    pub(crate) fn handle_confirm(&mut self, action: Action) -> Effect {
        match action {
            Action::Char(c) => {
                if self.confirm.typed.len() < 128 {
                    self.confirm.typed.push(c);
                }
                self.confirm.error = None;
                Effect::None
            }
            Action::Backspace => {
                self.confirm.typed.pop();
                self.confirm.error = None;
                Effect::None
            }
            Action::ClearInput => {
                self.confirm.typed.clear();
                self.confirm.error = None;
                Effect::None
            }
            Action::Confirm => {
                let Some(card) = self.selected_card() else {
                    self.confirm.error = Some("No card selected.".to_owned());
                    return Effect::None;
                };
                if !card.matches_confirm(&self.confirm.typed) {
                    let expected = card.effective_confirm_token();
                    self.confirm.error = Some(format!("Expected \"{expected}\" exactly before writing to the card."));
                    return Effect::None;
                }
                let selection = Selection {
                    mode: self.mode,
                    build_id: self.selected_build().map(|b| b.id.clone()),
                    card_id: card.id.clone(),
                    ios_size_bytes: self
                        .mode
                        .needs_size()
                        .then(|| self.size.as_ref().map(|s| s.selected_bytes))
                        .flatten(),
                    resume_journal: false,
                };
                self.outcome = Some(WizardOutcome::Confirmed(selection.clone()));
                Effect::WizardConfirmed(selection)
            }
            Action::Back => {
                self.confirm = ConfirmState::default();
                self.screen = if self.mode.needs_size() {
                    Screen::PickSize
                } else {
                    Screen::PickCard
                };
                Effect::None
            }
            Action::Quit => self.cancel_wizard(),
            _ => Effect::None,
        }
    }

    pub(crate) fn handle_progress(&mut self, action: Action) -> Effect {
        if self.handle_log_scroll(&action) {
            return Effect::None;
        }
        match action {
            Action::Quit => {
                if self.progress.irreversible {
                    self.progress.irreversible_warning = true;
                    Effect::None
                } else {
                    self.progress.cancel_requested = true;
                    Effect::RequestCancel
                }
            }
            _ => Effect::None,
        }
    }

    pub(crate) fn handle_final(&mut self, action: Action) -> Effect {
        if self.handle_log_scroll(&action) {
            return Effect::None;
        }
        match action {
            Action::Quit | Action::Confirm => Effect::Exit,
            _ => Effect::None,
        }
    }

    fn handle_log_scroll(&mut self, action: &Action) -> bool {
        match action {
            Action::Up => self.progress.scroll_logs_up(1),
            Action::Down => self.progress.scroll_logs_down(1),
            Action::PageUp => self.progress.scroll_logs_up(8),
            Action::PageDown => self.progress.scroll_logs_down(8),
            Action::Home => self.progress.scroll_logs_top(),
            Action::End => self.progress.scroll_logs_bottom(),
            _ => return false,
        }
        true
    }

    fn cancel_wizard(&mut self) -> Effect {
        self.outcome = Some(WizardOutcome::Cancelled);
        Effect::WizardCancelled
    }
}
