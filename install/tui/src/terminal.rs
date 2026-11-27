// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Stdout, stdout};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use crossterm::cursor::{Hide, Show};
use crossterm::event::{self, Event};
use crossterm::execute;
use crossterm::terminal::{EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode};
use crossterm::tty::IsTty;
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;

use crate::model::{
    CardOption, DoneSummary, ErrorSummary, Mode, ProgressEvent, Selection, StepDescriptor, TuiError, WizardConfig,
    WizardOutcome,
};
use crate::render::render;
use crate::state::{Action, App, Effect, Screen};

const TICK_RATE: Duration = Duration::from_millis(50);

#[derive(Clone, Debug, Default)]
pub struct CancelToken {
    cancelled: Arc<AtomicBool>,
}

impl CancelToken {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::SeqCst);
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::SeqCst)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FinalStatus {
    CancelledInWizard,
    Done(DoneSummary),
    Error(ErrorSummary),
}

struct TerminalGuard {
    terminal: Terminal<CrosstermBackend<Stdout>>,
}

impl TerminalGuard {
    fn enter() -> Result<Self, TuiError> {
        if !stdout().is_tty() {
            return Err(TuiError::NotATty);
        }
        enable_raw_mode()?;
        let mut out = stdout();
        if let Err(err) = execute!(out, EnterAlternateScreen, Hide) {
            let _ = disable_raw_mode();
            return Err(TuiError::Io(err));
        }
        let backend = CrosstermBackend::new(out);
        let terminal = match Terminal::new(backend) {
            Ok(t) => t,
            Err(err) => {
                let mut out = io::stdout();
                let _ = execute!(out, LeaveAlternateScreen, Show);
                let _ = disable_raw_mode();
                return Err(TuiError::Io(err));
            }
        };
        Ok(Self { terminal })
    }
}

impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let mut out = io::stdout();
        let _ = execute!(out, LeaveAlternateScreen, Show);
        let _ = disable_raw_mode();
    }
}

/// Runs the full interactive wizard, transitions into the progress screen when confirmed, and
/// displays the completion or error summary before exiting.
pub fn run_terminal<R, S>(config: WizardConfig, mut refresh_cards: R, start_work: S) -> Result<FinalStatus, TuiError>
where
    R: FnMut() -> Result<Vec<CardOption>, String>,
    S: FnOnce(Selection, CancelToken) -> (Vec<StepDescriptor>, Receiver<ProgressEvent>),
{
    let mut guard = TerminalGuard::enter()?;
    let mut app = App::new(config);
    let mut start_work = Some(start_work);
    let mut progress_rx: Option<Receiver<ProgressEvent>> = None;
    let cancel = CancelToken::new();
    let mut last_tick = Instant::now();

    loop {
        if let Some(rx) = &progress_rx {
            app.drain_progress(rx);
        }

        guard.terminal.draw(|frame| render(frame, &app))?;

        let timeout = TICK_RATE.saturating_sub(last_tick.elapsed());
        if event::poll(timeout)?
            && let Event::Key(key) = event::read()?
        {
            match app.handle_key(key) {
                Effect::None => {}
                Effect::RefreshCards => match refresh_cards() {
                    Ok(cards) => app.set_cards(cards),
                    Err(err) => app.set_notice(format!("Refresh failed: {err}")),
                },
                Effect::WizardConfirmed(selection) => {
                    if let Some(starter) = start_work.take() {
                        let (steps, rx) = starter(selection, cancel.clone());
                        app.begin_progress(steps);
                        progress_rx = Some(rx);
                        last_tick = Instant::now();
                    }
                }
                Effect::WizardCancelled => return Ok(FinalStatus::CancelledInWizard),
                Effect::RequestCancel => cancel.cancel(),
                Effect::Exit => return Ok(final_status_from_app(&app)),
            }
        }

        let elapsed = last_tick.elapsed();
        if elapsed >= TICK_RATE {
            app.handle_action(Action::Tick(elapsed));
            last_tick = Instant::now();
        }
    }
}

/// Runs only the wizard screens and returns the user's confirmed [`Selection`] or cancellation.
pub fn run_wizard_terminal<R>(config: WizardConfig, mut refresh_cards: R) -> Result<WizardOutcome, TuiError>
where
    R: FnMut() -> Result<Vec<CardOption>, String>,
{
    let mut guard = TerminalGuard::enter()?;
    let mut app = App::new(config);

    loop {
        guard.terminal.draw(|frame| render(frame, &app))?;
        if event::poll(TICK_RATE)?
            && let Event::Key(key) = event::read()?
        {
            match app.handle_key(key) {
                Effect::None | Effect::RequestCancel | Effect::Exit => {}
                Effect::RefreshCards => match refresh_cards() {
                    Ok(cards) => app.set_cards(cards),
                    Err(err) => app.set_notice(format!("Refresh failed: {err}")),
                },
                Effect::WizardConfirmed(selection) => return Ok(WizardOutcome::Confirmed(selection)),
                Effect::WizardCancelled => return Ok(WizardOutcome::Cancelled),
            }
        }
    }
}

/// Runs the progress, completion, and error screens directly (for non-interactive CLI runs).
pub fn run_progress_terminal(
    title: impl Into<String>,
    mode: Mode,
    steps: Vec<StepDescriptor>,
    rx: Receiver<ProgressEvent>,
    cancel: CancelToken,
) -> Result<FinalStatus, TuiError> {
    let mut guard = TerminalGuard::enter()?;
    let mut app = App::new_progress(title, mode, steps);
    let mut last_tick = Instant::now();

    loop {
        app.drain_progress(&rx);
        guard.terminal.draw(|frame| render(frame, &app))?;

        let timeout = TICK_RATE.saturating_sub(last_tick.elapsed());
        if event::poll(timeout)?
            && let Event::Key(key) = event::read()?
        {
            match app.handle_key(key) {
                Effect::RequestCancel => cancel.cancel(),
                Effect::Exit => return Ok(final_status_from_app(&app)),
                _ => {}
            }
        }

        let elapsed = last_tick.elapsed();
        if elapsed >= TICK_RATE {
            app.handle_action(Action::Tick(elapsed));
            last_tick = Instant::now();
        }
    }
}

fn final_status_from_app(app: &App) -> FinalStatus {
    match app.screen() {
        Screen::Done => FinalStatus::Done(app.done_summary().cloned().expect("done screen has summary")),
        Screen::Error => FinalStatus::Error(app.error_summary().cloned().expect("error screen has summary")),
        _ => FinalStatus::CancelledInWizard,
    }
}
