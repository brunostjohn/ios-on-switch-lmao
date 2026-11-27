// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::error::{EngineError, Result};
use crate::paths::RunLog;

#[derive(Clone, Debug, Default)]
pub struct CancelToken {
    flag: Arc<AtomicBool>,
}

impl CancelToken {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn from_arc(flag: Arc<AtomicBool>) -> Self {
        Self { flag }
    }

    pub fn into_arc(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.flag)
    }

    pub fn cancel(&self) {
        self.flag.store(true, Ordering::SeqCst);
    }

    pub fn is_cancelled(&self) -> bool {
        self.flag.load(Ordering::SeqCst)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EngineStep {
    pub id: String,
    pub label: String,
}

impl EngineStep {
    pub fn new(id: impl Into<String>, label: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepOutcome {
    Done,
    Failed,
    Skipped,
}

#[derive(Clone, Debug, PartialEq)]
pub enum EngineEvent {
    SetSteps(Vec<EngineStep>),
    StepStarted {
        step_id: String,
        irreversible: bool,
    },
    StepProgress {
        step_id: String,
        done_bytes: u64,
        total_bytes: Option<u64>,
        bytes_per_sec: Option<f64>,
        detail: Option<String>,
    },
    StepFinished {
        step_id: String,
        outcome: StepOutcome,
        elapsed: Duration,
    },
    SetIrreversible(bool),
    Log(String),
}

pub trait ProgressSink: Send {
    fn emit(&mut self, event: EngineEvent);
}

#[derive(Default)]
pub struct NoopSink;

impl ProgressSink for NoopSink {
    fn emit(&mut self, _event: EngineEvent) {}
}

impl<F> ProgressSink for F
where
    F: FnMut(EngineEvent) + Send,
{
    fn emit(&mut self, event: EngineEvent) {
        self(event);
    }
}

pub struct EngineProgress<'a> {
    sink: &'a mut dyn ProgressSink,
    log: Option<RunLog>,
    cancel: CancelToken,
    irreversible: bool,
    active_step: Option<(String, Instant)>,
    last_emit: Instant,
    rate_anchor: Option<(Instant, u64)>,
}

impl<'a> EngineProgress<'a> {
    pub fn new(sink: &'a mut dyn ProgressSink, log: Option<RunLog>, cancel: CancelToken) -> Self {
        Self {
            sink,
            log,
            cancel,
            irreversible: false,
            active_step: None,
            last_emit: Instant::now(),
            rate_anchor: None,
        }
    }

    pub fn cancel_token(&self) -> CancelToken {
        self.cancel.clone()
    }

    pub fn is_irreversible(&self) -> bool {
        self.irreversible
    }

    pub fn log_path(&self) -> Option<PathBuf> {
        self.log.as_ref().map(|l| l.path().to_path_buf())
    }

    pub fn active_step_id(&self) -> Option<&str> {
        self.active_step.as_ref().map(|(id, _)| id.as_str())
    }

    /// Cancels only before irreversible card writes begin; once writing starts, finishing safely takes precedence.
    pub fn check_cancel(&self) -> Result<()> {
        if !self.irreversible && self.cancel.is_cancelled() {
            return Err(EngineError::Cancelled);
        }
        Ok(())
    }

    pub fn set_steps(&mut self, steps: Vec<EngineStep>) {
        self.sink.emit(EngineEvent::SetSteps(steps));
    }

    pub fn set_irreversible(&mut self, flag: bool) {
        self.irreversible = flag;
        if flag {
            self.write_log("card writes started (irreversible)");
        }
        self.sink.emit(EngineEvent::SetIrreversible(flag));
    }

    pub fn log(&mut self, msg: impl Into<String>) {
        let msg = msg.into();
        self.write_log(&msg);
        self.sink.emit(EngineEvent::Log(msg));
    }

    pub fn start_step(&mut self, step_id: &str, irreversible: bool) -> Result<()> {
        if !irreversible {
            self.check_cancel()?;
        } else {
            self.check_cancel()?;
            self.irreversible = true;
        }
        let now = Instant::now();
        self.active_step = Some((step_id.to_owned(), now));
        self.last_emit = now;
        self.rate_anchor = Some((now, 0));
        self.write_log(&format!("step start: {step_id} (irreversible={irreversible})"));
        self.sink.emit(EngineEvent::StepStarted {
            step_id: step_id.to_owned(),
            irreversible,
        });
        Ok(())
    }

    pub fn step_progress(
        &mut self,
        step_id: &str,
        done_bytes: u64,
        total_bytes: Option<u64>,
        detail: Option<String>,
        force: bool,
    ) {
        let now = Instant::now();
        if !force && now.duration_since(self.last_emit) < Duration::from_millis(75) {
            return;
        }
        let rate = self.rate_anchor.and_then(|(t0, b0)| {
            let dt = now.duration_since(t0).as_secs_f64();
            if dt >= 0.05 && done_bytes >= b0 {
                Some((done_bytes - b0) as f64 / dt)
            } else {
                None
            }
        });
        self.last_emit = now;
        self.sink.emit(EngineEvent::StepProgress {
            step_id: step_id.to_owned(),
            done_bytes,
            total_bytes,
            bytes_per_sec: rate,
            detail,
        });
    }

    pub fn finish_step(&mut self, step_id: &str, outcome: StepOutcome) {
        let elapsed = self
            .active_step
            .take()
            .filter(|(id, _)| id == step_id)
            .map_or(Duration::ZERO, |(_, t)| t.elapsed());
        self.rate_anchor = None;
        self.write_log(&format!("step finish: {step_id} ({outcome:?}, {:.2?})", elapsed));
        self.sink.emit(EngineEvent::StepFinished {
            step_id: step_id.to_owned(),
            outcome,
            elapsed,
        });
    }

    pub fn fail_active_step(&mut self) {
        if let Some((id, started)) = self.active_step.take() {
            let elapsed = started.elapsed();
            self.write_log(&format!("step failed: {id} ({elapsed:.2?})"));
            self.sink.emit(EngineEvent::StepFinished {
                step_id: id,
                outcome: StepOutcome::Failed,
                elapsed,
            });
        }
    }

    fn write_log(&mut self, msg: &str) {
        if let Some(log) = &mut self.log {
            log.write_line(msg);
        }
    }
}
