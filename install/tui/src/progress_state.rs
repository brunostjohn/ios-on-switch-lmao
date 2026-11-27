// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::VecDeque;
use std::time::Duration;

use crate::model::{StepDescriptor, StepState, StepStatus};

pub const MAX_LOG_LINES: usize = 2000;

const SPINNER: [&str; 10] = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];

#[derive(Clone, Debug, PartialEq)]
pub struct ActiveProgress {
    pub step_id: String,
    pub done_bytes: u64,
    pub total_bytes: Option<u64>,
    pub bytes_per_sec: Option<f64>,
    pub eta: Option<Duration>,
    pub detail: Option<String>,
}

#[derive(Clone, Debug)]
pub struct ProgressState {
    pub steps: Vec<StepState>,
    pub active: Option<ActiveProgress>,
    pub active_elapsed: Duration,
    pub total_elapsed: Duration,
    pub irreversible: bool,
    pub cancel_requested: bool,
    pub irreversible_warning: bool,
    pub logs: VecDeque<String>,
    pub follow_logs: bool,
    pub scroll_from_bottom: usize,
    tick_count: usize,
}

impl ProgressState {
    pub fn new(steps: Vec<StepDescriptor>) -> Self {
        Self {
            steps: steps.into_iter().map(step_from_descriptor).collect(),
            active: None,
            active_elapsed: Duration::ZERO,
            total_elapsed: Duration::ZERO,
            irreversible: false,
            cancel_requested: false,
            irreversible_warning: false,
            logs: VecDeque::new(),
            follow_logs: true,
            scroll_from_bottom: 0,
            tick_count: 0,
        }
    }

    pub fn set_steps(&mut self, steps: Vec<StepDescriptor>) {
        self.steps = steps.into_iter().map(step_from_descriptor).collect();
        self.active = None;
        self.active_elapsed = Duration::ZERO;
    }

    pub fn step_started(&mut self, step_id: String, irreversible: bool) {
        if let Some(step) = self.steps.iter_mut().find(|s| s.id == step_id) {
            step.status = StepStatus::Active;
            step.elapsed = Duration::ZERO;
            step.detail = None;
        }
        self.active = Some(ActiveProgress {
            step_id,
            done_bytes: 0,
            total_bytes: None,
            bytes_per_sec: None,
            eta: None,
            detail: None,
        });
        self.active_elapsed = Duration::ZERO;
        self.irreversible = irreversible;
        if !irreversible {
            self.irreversible_warning = false;
        }
    }

    pub fn step_progress(
        &mut self,
        step_id: String,
        done_bytes: u64,
        total_bytes: Option<u64>,
        explicit_bps: Option<f64>,
        detail: Option<String>,
    ) {
        if let Some(step) = self.steps.iter_mut().find(|s| s.id == step_id) {
            if step.status == StepStatus::Pending {
                step.status = StepStatus::Active;
            }
            if detail.is_some() {
                step.detail = detail.clone();
            }
        }

        let rate = explicit_bps.filter(|v| v.is_finite() && *v >= 0.0).or_else(|| {
            let secs = self.active_elapsed.as_secs_f64();
            if secs >= 0.1 && done_bytes > 0 {
                Some(done_bytes as f64 / secs)
            } else {
                None
            }
        });

        let eta = match (total_bytes, rate) {
            (Some(total), Some(bps)) if total > done_bytes && bps > 1.0 => {
                let rem_secs = (total - done_bytes) as f64 / bps;
                if rem_secs.is_finite() && rem_secs >= 0.0 {
                    Some(Duration::from_secs_f64(rem_secs.min(86_400.0 * 7.0)))
                } else {
                    None
                }
            }
            _ => None,
        };

        self.active = Some(ActiveProgress {
            step_id,
            done_bytes,
            total_bytes,
            bytes_per_sec: rate,
            eta,
            detail,
        });
    }

    pub fn step_finished(&mut self, step_id: &str, status: StepStatus, elapsed: Option<Duration>) {
        if let Some(step) = self.steps.iter_mut().find(|s| s.id == step_id) {
            step.status = status;
            if let Some(d) = elapsed {
                step.elapsed = d;
            }
        }
        if self.active.as_ref().is_some_and(|a| a.step_id == step_id) && status != StepStatus::Active {
            self.active = None;
        }
    }

    pub fn set_irreversible(&mut self, irreversible: bool) {
        self.irreversible = irreversible;
        if !irreversible {
            self.irreversible_warning = false;
        }
    }

    pub fn push_log(&mut self, text: &str) {
        for raw_line in text.split('\n') {
            let line = raw_line.trim_end_matches('\r').to_owned();
            if self.logs.len() >= MAX_LOG_LINES {
                self.logs.pop_front();
            }
            self.logs.push_back(line);
            if !self.follow_logs {
                self.scroll_from_bottom = self
                    .scroll_from_bottom
                    .saturating_add(1)
                    .min(self.logs.len().saturating_sub(1));
            }
        }
    }

    pub fn scroll_logs_up(&mut self, lines: usize) {
        if self.logs.is_empty() {
            return;
        }
        self.follow_logs = false;
        self.scroll_from_bottom = self
            .scroll_from_bottom
            .saturating_add(lines)
            .min(self.logs.len().saturating_sub(1));
    }

    pub fn scroll_logs_down(&mut self, lines: usize) {
        if self.scroll_from_bottom <= lines {
            self.scroll_from_bottom = 0;
            self.follow_logs = true;
        } else {
            self.scroll_from_bottom -= lines;
        }
    }

    pub fn scroll_logs_top(&mut self) {
        if self.logs.is_empty() {
            return;
        }
        self.follow_logs = false;
        self.scroll_from_bottom = self.logs.len().saturating_sub(1);
    }

    pub fn scroll_logs_bottom(&mut self) {
        self.scroll_from_bottom = 0;
        self.follow_logs = true;
    }

    pub fn tick(&mut self, delta: Duration) {
        self.tick_count = self.tick_count.wrapping_add(1);
        self.total_elapsed = self.total_elapsed.saturating_add(delta);
        if let Some(active_id) = self.active.as_ref().map(|a| a.step_id.clone()) {
            self.active_elapsed = self.active_elapsed.saturating_add(delta);
            if let Some(step) = self.steps.iter_mut().find(|s| s.id == active_id)
                && step.status == StepStatus::Active
            {
                step.elapsed = step.elapsed.saturating_add(delta);
            }
        }
    }

    pub fn spinner_frame(&self) -> &'static str {
        SPINNER[self.tick_count % SPINNER.len()]
    }

    pub fn active_step_label(&self) -> Option<&str> {
        let active_id = self.active.as_ref().map(|a| a.step_id.as_str())?;
        self.steps.iter().find(|s| s.id == active_id).map(|s| s.label.as_str())
    }

    pub fn tick_count(&self) -> usize {
        self.tick_count
    }

    pub fn visible_log_slice(&self, viewport_rows: usize) -> (usize, usize) {
        let total = self.logs.len();
        if total == 0 || viewport_rows == 0 {
            return (0, 0);
        }
        let max_offset = total.saturating_sub(viewport_rows);
        let from_bottom = self.scroll_from_bottom.min(max_offset);
        let end = total.saturating_sub(from_bottom);
        let start = end.saturating_sub(viewport_rows);
        (start, end)
    }
}

fn step_from_descriptor(desc: StepDescriptor) -> StepState {
    StepState {
        id: desc.id,
        label: desc.label,
        status: StepStatus::Pending,
        elapsed: Duration::ZERO,
        detail: None,
    }
}
