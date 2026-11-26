// SPDX-License-Identifier: GPL-3.0-or-later
//! Interactive terminal UI for the iPadOS Switch installer.
//!
//! Pure state machine (`App`, `Action`, `Screen`, `render`) plus a `crossterm`/`ratatui` terminal
//! runner, kept decoupled from the installer engine and raw disk crates.

mod input;
mod model;
mod progress_state;
mod render;
mod screens;
mod size_state;
mod state;
mod terminal;
mod theme;
mod transitions;

pub use input::map_key;
pub use model::{
    ALIGN_BYTES, BuildOption, COARSE_STEP_BYTES, CardOption, CardStateAfterError, DEFAULT_IOS_BYTES, DoneSummary,
    ErrorSummary, FINE_STEP_BYTES, Mode, PAGE_STEP_BYTES, PendingJournal, ProgressEvent, RelocationEstimate, Selection,
    SizeConstraints, SlotStatus, StepDescriptor, StepState, StepStatus, TuiError, WizardConfig, WizardOutcome,
    align_down, align_up, format_bytes, format_duration, format_gib, snap_nearest,
};
pub use progress_state::{ActiveProgress, MAX_LOG_LINES, ProgressState};
pub use render::render;
pub use size_state::{SizeState, parse_gib_to_bytes};
pub use state::{Action, App, Breadcrumb, ConfirmState, Effect, ResumeState, Screen};
pub use terminal::{CancelToken, FinalStatus, run_progress_terminal, run_terminal, run_wizard_terminal};
