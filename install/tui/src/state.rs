// SPDX-License-Identifier: GPL-3.0-or-later
use std::sync::mpsc::{self, TryRecvError};
use std::time::Duration;

use crossterm::event::KeyEvent;

use crate::input::map_key;
use crate::model::{
    BuildOption, CardOption, CardStateAfterError, DoneSummary, ErrorSummary, Mode, ProgressEvent, Selection,
    StepDescriptor, WizardConfig, WizardOutcome,
};
use crate::progress_state::ProgressState;
use crate::size_state::SizeState;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Screen {
    PickMode,
    PickBuild,
    PickCard,
    ResumeJournal,
    PickSize,
    Confirm,
    Progress,
    Done,
    Error,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    FineDecrease,
    FineIncrease,
    Home,
    End,
    SelectIndex(usize),
    Confirm,
    Back,
    RefreshCards,
    Char(char),
    Backspace,
    ClearInput,
    Quit,
    Tick(Duration),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Effect {
    None,
    RefreshCards,
    WizardConfirmed(Selection),
    WizardCancelled,
    RequestCancel,
    Exit,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Breadcrumb {
    pub label: &'static str,
    pub active: bool,
    pub done: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResumeState {
    pub selected: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConfirmState {
    pub typed: String,
    pub error: Option<String>,
}

pub struct App {
    pub(crate) title: String,
    pub(crate) screen: Screen,
    pub(crate) mode_fixed: bool,
    pub(crate) mode_index: usize,
    pub(crate) mode: Mode,
    pub(crate) builds: Vec<BuildOption>,
    pub(crate) build_index: usize,
    pub(crate) cards: Vec<CardOption>,
    pub(crate) card_index: usize,
    pub(crate) resume: ResumeState,
    pub(crate) size: Option<SizeState>,
    pub(crate) size_card_id: Option<String>,
    pub(crate) size_build_id: Option<String>,
    pub(crate) confirm: ConfirmState,
    pub(crate) progress: ProgressState,
    pub(crate) done: Option<DoneSummary>,
    pub(crate) error: Option<ErrorSummary>,
    pub(crate) outcome: Option<WizardOutcome>,
    pub(crate) notice: Option<String>,
}

impl App {
    pub fn new(config: WizardConfig) -> Self {
        let mode_fixed = config.initial_mode.is_some();
        let mode = config.initial_mode.unwrap_or(Mode::Install);
        let mode_index = Mode::ALL.iter().position(|&m| m == mode).unwrap_or(0);
        let build_index = first_supported_build(&config.builds);
        let screen = match config.initial_mode {
            None => Screen::PickMode,
            Some(m) if m.needs_build() => Screen::PickBuild,
            Some(_) => Screen::PickCard,
        };
        Self {
            title: config.title,
            screen,
            mode_fixed,
            mode_index,
            mode,
            builds: config.builds,
            build_index,
            cards: config.cards,
            card_index: 0,
            resume: ResumeState { selected: 0 },
            size: None,
            size_card_id: None,
            size_build_id: None,
            confirm: ConfirmState::default(),
            progress: ProgressState::new(Vec::new()),
            done: None,
            error: None,
            outcome: None,
            notice: None,
        }
    }

    pub fn new_progress(title: impl Into<String>, mode: Mode, steps: Vec<StepDescriptor>) -> Self {
        let mode_index = Mode::ALL.iter().position(|&m| m == mode).unwrap_or(0);
        Self {
            title: title.into(),
            screen: Screen::Progress,
            mode_fixed: true,
            mode_index,
            mode,
            builds: Vec::new(),
            build_index: 0,
            cards: Vec::new(),
            card_index: 0,
            resume: ResumeState { selected: 0 },
            size: None,
            size_card_id: None,
            size_build_id: None,
            confirm: ConfirmState::default(),
            progress: ProgressState::new(steps),
            done: None,
            error: None,
            outcome: None,
            notice: None,
        }
    }

    pub fn title(&self) -> &str {
        &self.title
    }

    pub fn screen(&self) -> Screen {
        self.screen
    }

    pub fn mode(&self) -> Mode {
        self.mode
    }

    pub fn mode_index(&self) -> usize {
        self.mode_index
    }

    pub fn mode_fixed(&self) -> bool {
        self.mode_fixed
    }

    pub fn builds(&self) -> &[BuildOption] {
        &self.builds
    }

    pub fn build_index(&self) -> usize {
        self.build_index
    }

    pub fn selected_build(&self) -> Option<&BuildOption> {
        if self.mode.needs_build() {
            self.builds.get(self.build_index)
        } else {
            None
        }
    }

    pub fn cards(&self) -> &[CardOption] {
        &self.cards
    }

    pub fn card_index(&self) -> usize {
        self.card_index
    }

    pub fn selected_card(&self) -> Option<&CardOption> {
        self.cards.get(self.card_index)
    }

    pub fn resume_state(&self) -> ResumeState {
        self.resume
    }

    pub fn size_state(&self) -> Option<&SizeState> {
        self.size.as_ref()
    }

    pub fn confirm_state(&self) -> &ConfirmState {
        &self.confirm
    }

    pub fn progress_state(&self) -> &ProgressState {
        &self.progress
    }

    pub fn done_summary(&self) -> Option<&DoneSummary> {
        self.done.as_ref()
    }

    pub fn error_summary(&self) -> Option<&ErrorSummary> {
        self.error.as_ref()
    }

    pub fn outcome(&self) -> Option<&WizardOutcome> {
        self.outcome.as_ref()
    }

    pub fn notice(&self) -> Option<&str> {
        self.notice.as_deref()
    }

    pub fn set_notice(&mut self, msg: impl Into<String>) {
        self.notice = Some(msg.into());
    }

    pub fn clear_notice(&mut self) {
        self.notice = None;
    }

    pub fn set_cards(&mut self, cards: Vec<CardOption>) {
        let prev_id = self.cards.get(self.card_index).map(|c| c.id.clone());
        let count = cards.len();
        self.cards = cards;
        self.card_index = prev_id
            .and_then(|id| self.cards.iter().position(|c| c.id == id))
            .unwrap_or(0)
            .min(self.cards.len().saturating_sub(1));
        self.size = None;
        self.size_card_id = None;
        self.notice = Some(if count == 0 {
            "Refreshed: no removable microSD cards found".to_owned()
        } else if count == 1 {
            "Refreshed: 1 removable card found".to_owned()
        } else {
            format!("Refreshed: {count} removable cards found")
        });
    }

    pub fn begin_progress(&mut self, steps: Vec<StepDescriptor>) {
        self.progress = ProgressState::new(steps);
        self.done = None;
        self.error = None;
        self.notice = None;
        self.screen = Screen::Progress;
    }

    pub fn handle_key(&mut self, key: KeyEvent) -> Effect {
        match map_key(self.screen, key) {
            Some(action) => self.handle_action(action),
            None => Effect::None,
        }
    }

    pub fn handle_action(&mut self, action: Action) -> Effect {
        if let Action::Tick(delta) = action {
            if self.screen == Screen::Progress {
                self.progress.tick(delta);
            }
            return Effect::None;
        }

        match self.screen {
            Screen::PickMode => self.handle_pick_mode(action),
            Screen::PickBuild => self.handle_pick_build(action),
            Screen::PickCard => self.handle_pick_card(action),
            Screen::ResumeJournal => self.handle_resume(action),
            Screen::PickSize => self.handle_pick_size(action),
            Screen::Confirm => self.handle_confirm(action),
            Screen::Progress => self.handle_progress(action),
            Screen::Done | Screen::Error => self.handle_final(action),
        }
    }

    pub fn apply_progress_event(&mut self, event: ProgressEvent) {
        match event {
            ProgressEvent::SetSteps(steps) => self.progress.set_steps(steps),
            ProgressEvent::StepStarted { step_id, irreversible } => {
                self.progress.step_started(step_id, irreversible);
            }
            ProgressEvent::StepProgress {
                step_id,
                done_bytes,
                total_bytes,
                bytes_per_sec,
                detail,
            } => {
                self.progress
                    .step_progress(step_id, done_bytes, total_bytes, bytes_per_sec, detail);
            }
            ProgressEvent::StepFinished {
                step_id,
                status,
                elapsed,
            } => {
                self.progress.step_finished(&step_id, status, elapsed);
            }
            ProgressEvent::SetIrreversible(flag) => self.progress.set_irreversible(flag),
            ProgressEvent::Log(line) => self.progress.push_log(&line),
            ProgressEvent::Finished(summary) => {
                self.done = Some(summary);
                self.error = None;
                self.screen = Screen::Done;
            }
            ProgressEvent::Failed(summary) => {
                self.error = Some(summary);
                self.done = None;
                self.screen = Screen::Error;
            }
            ProgressEvent::Cancelled { message, log_path } => {
                self.error = Some(ErrorSummary {
                    title: "Operation cancelled".to_owned(),
                    message,
                    log_path,
                    card_state: CardStateAfterError::Untouched,
                    failed_step: None,
                });
                self.done = None;
                self.screen = Screen::Error;
            }
        }
    }

    /// Drains pending progress events. Returns `true` if the sender disconnected while still running.
    pub fn drain_progress(&mut self, rx: &mpsc::Receiver<ProgressEvent>) -> bool {
        loop {
            match rx.try_recv() {
                Ok(event) => self.apply_progress_event(event),
                Err(TryRecvError::Empty) => return false,
                Err(TryRecvError::Disconnected) => {
                    if self.screen == Screen::Progress {
                        let card_state = if self.progress.irreversible {
                            CardStateAfterError::Partial {
                                detail: "Worker ended while writing to the card.".to_owned(),
                            }
                        } else {
                            CardStateAfterError::Untouched
                        };
                        self.error = Some(ErrorSummary {
                            title: "Operation interrupted".to_owned(),
                            message: "The installer worker exited before reporting a result.".to_owned(),
                            log_path: None,
                            card_state,
                            failed_step: self.progress.active_step_label().map(str::to_owned),
                        });
                        self.screen = Screen::Error;
                        return true;
                    }
                    return false;
                }
            }
        }
    }

    pub fn breadcrumbs(&self) -> Vec<Breadcrumb> {
        let mut steps: Vec<(Screen, &'static str)> = Vec::new();
        if !self.mode_fixed {
            steps.push((Screen::PickMode, "Mode"));
        }
        if self.mode.needs_build() {
            steps.push((Screen::PickBuild, "Build"));
        }
        steps.push((Screen::PickCard, "Card"));
        if self.screen == Screen::ResumeJournal {
            steps.push((Screen::ResumeJournal, "Resume"));
        } else {
            if self.mode.needs_size() {
                steps.push((Screen::PickSize, "Size"));
            }
            steps.push((Screen::Confirm, "Confirm"));
        }
        steps.push((
            Screen::Progress,
            match self.mode {
                Mode::Partition => "Partition",
                Mode::Install | Mode::PartitionAndInstall => "Install",
            },
        ));

        let current_pos = match self.screen {
            Screen::Done | Screen::Error => steps.len(),
            s => steps.iter().position(|(sc, _)| *sc == s).unwrap_or(0),
        };

        steps
            .into_iter()
            .enumerate()
            .map(|(i, (_, label))| Breadcrumb {
                label,
                active: i == current_pos,
                done: i < current_pos,
            })
            .collect()
    }
}

fn first_supported_build(builds: &[BuildOption]) -> usize {
    builds.iter().position(BuildOption::is_supported).unwrap_or(0)
}
