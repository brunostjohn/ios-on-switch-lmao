// SPDX-License-Identifier: GPL-3.0-or-later
use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Gauge, Paragraph, Wrap};

use crate::model::{CardStateAfterError, StepStatus, format_bytes, format_duration};
use crate::progress_state::ProgressState;
use crate::screens::panel;
use crate::state::App;
use crate::theme;

const MIB_F64: f64 = 1024.0 * 1024.0;

pub fn render_progress(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let progress = app.progress_state();
    let has_banner = progress.irreversible_warning || progress.cancel_requested;
    let banner_h = if has_banner { 2 } else { 0 };
    let gauge_h = 3;

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(banner_h),
            Constraint::Length(gauge_h),
            Constraint::Min(6),
        ])
        .split(area);

    if has_banner {
        render_progress_banner(frame, chunks[0], progress);
    }
    render_active_bar(frame, chunks[1], progress);

    let step_rows = (progress.steps.len() as u16).saturating_add(2);
    let body_h = chunks[2].height;
    let steps_h = if compact {
        step_rows.clamp(4, body_h.saturating_sub(3).max(4))
    } else {
        step_rows.clamp(5, (body_h * 55 / 100).max(5))
    };

    let lower = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(steps_h), Constraint::Min(3)])
        .split(chunks[2]);

    render_steps(frame, lower[0], progress);
    render_logs(frame, lower[1], progress);
}

fn render_progress_banner(frame: &mut Frame<'_>, area: Rect, progress: &ProgressState) {
    let line = if progress.irreversible_warning {
        Line::from(vec![
            Span::styled("WARNING: ", theme::err_bold()),
            Span::styled(
                "Card writes are in progress — do NOT unplug the microSD card or interrupt!",
                theme::warn_bold(),
            ),
        ])
    } else {
        Line::from(vec![
            Span::styled("Cancelling: ", theme::warn_bold()),
            Span::styled("Stopping cleanly at the next safe checkpoint...", theme::warn()),
        ])
    };
    frame.render_widget(Paragraph::new(line).wrap(Wrap { trim: true }), area);
}

fn render_active_bar(frame: &mut Frame<'_>, area: Rect, progress: &ProgressState) {
    let title = progress
        .active_step_label()
        .map(|s| format!(" {s} "))
        .unwrap_or_else(|| " Working ".to_owned());

    let Some(active) = &progress.active else {
        let line = Line::from(vec![
            Span::styled(progress.spinner_frame(), theme::accent()),
            Span::raw(" Preparing..."),
        ]);
        frame.render_widget(Paragraph::new(line).block(panel(title.trim())), area);
        return;
    };

    if let Some(total) = active.total_bytes
        && total > 0
    {
        let ratio = (active.done_bytes as f64 / total as f64).clamp(0.0, 1.0);
        let pct = ratio * 100.0;
        let mut label = format!(
            "{pct:5.1}% · {} / {}",
            format_bytes(active.done_bytes),
            format_bytes(total)
        );
        if let Some(bps) = active.bytes_per_sec
            && bps > 0.0
        {
            label.push_str(&format!(" · {:.1} MiB/s", bps / MIB_F64));
        }
        if let Some(eta) = active.eta {
            label.push_str(&format!(" · ETA {}", format_duration(eta)));
        }
        let gauge = Gauge::default()
            .block(panel(title.trim()))
            .gauge_style(if progress.irreversible {
                theme::warn()
            } else {
                theme::accent()
            })
            .ratio(ratio)
            .label(label);
        frame.render_widget(gauge, area);
    } else {
        let bar_w = area.width.saturating_sub(24).clamp(6, 24) as usize;
        let pos = progress.tick_count() % bar_w;
        let mut track = String::with_capacity(bar_w);
        for i in 0..bar_w {
            if i == pos || (i + 1) % bar_w == pos {
                track.push('━');
            } else {
                track.push('─');
            }
        }
        let mut spans = vec![
            Span::styled(progress.spinner_frame(), theme::accent()),
            Span::raw(" "),
            Span::styled(track, theme::accent()),
        ];
        if active.done_bytes > 0 {
            spans.push(Span::raw(format!(" {}", format_bytes(active.done_bytes))));
        }
        if let Some(detail) = &active.detail {
            spans.push(Span::raw("  "));
            spans.push(Span::styled(detail.as_str(), theme::muted()));
        }
        frame.render_widget(Paragraph::new(Line::from(spans)).block(panel(title.trim())), area);
    }
}

fn render_steps(frame: &mut Frame<'_>, area: Rect, progress: &ProgressState) {
    let visible = area.height.saturating_sub(2) as usize;
    let total = progress.steps.len();
    let active_idx = progress
        .steps
        .iter()
        .position(|s| s.status == StepStatus::Active)
        .unwrap_or_else(|| total.saturating_sub(1));
    let start = if total <= visible {
        0
    } else {
        active_idx
            .saturating_sub(visible / 2)
            .min(total.saturating_sub(visible))
    };

    let lines: Vec<Line<'_>> = progress
        .steps
        .iter()
        .skip(start)
        .take(visible)
        .map(|step| {
            let (glyph, glyph_style, label_style) = match step.status {
                StepStatus::Pending => ("○", theme::muted(), theme::muted()),
                StepStatus::Active => (progress.spinner_frame(), theme::accent(), theme::heading()),
                StepStatus::Done => ("✓", theme::ok_bold(), theme::ok()),
                StepStatus::Failed => ("✗", theme::err_bold(), theme::err_bold()),
                StepStatus::Skipped => ("–", theme::muted(), theme::dimmed()),
            };
            let time = match step.status {
                StepStatus::Pending | StepStatus::Skipped => "     ".to_owned(),
                _ => format_duration(step.elapsed),
            };
            let mut spans = vec![
                Span::styled(glyph, glyph_style),
                Span::raw(" "),
                Span::styled(&step.label, label_style),
                Span::raw("  "),
                Span::styled(time, theme::muted()),
            ];
            if let Some(detail) = &step.detail
                && step.status == StepStatus::Active
            {
                spans.push(Span::raw("  "));
                spans.push(Span::styled(detail.as_str(), theme::muted()));
            }
            Line::from(spans)
        })
        .collect();

    let title = format!("Steps ({})", format_duration(progress.total_elapsed));
    frame.render_widget(Paragraph::new(lines).block(panel(&title)), area);
}

fn render_logs(frame: &mut Frame<'_>, area: Rect, progress: &ProgressState) {
    let inner_rows = area.height.saturating_sub(2) as usize;
    let (start, end) = progress.visible_log_slice(inner_rows);
    let lines: Vec<Line<'_>> = progress
        .logs
        .iter()
        .skip(start)
        .take(end.saturating_sub(start))
        .map(|l| Line::from(Span::styled(l.as_str(), theme::muted())))
        .collect();

    let title = if progress.follow_logs {
        "Log".to_owned()
    } else {
        format!("Log (paused, line {end}/{})", progress.logs.len())
    };
    frame.render_widget(Paragraph::new(lines).block(panel(&title)), area);
}

pub fn render_done(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let Some(done) = app.done_summary() else {
        return;
    };

    let summary_h = if compact { 6 } else { 8 };
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(summary_h), Constraint::Min(5)])
        .split(area);

    let ok_block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border_ok())
        .title(Span::styled(" Complete ", theme::ok_bold()));

    let eject_line = if done.ejected {
        Line::from(vec![
            Span::styled("✓ ", theme::ok_bold()),
            Span::styled("The microSD card has been safely ejected.", theme::ok()),
        ])
    } else {
        Line::from(vec![
            Span::styled("! ", theme::warn_bold()),
            Span::styled("Safely eject the microSD card before removing it.", theme::warn()),
        ])
    };

    let mut summary_lines = vec![
        Line::from(Span::styled(&done.title, theme::ok_bold())),
        Line::from(vec![
            Span::styled("Card: ", theme::muted()),
            Span::styled(&done.card_label, theme::heading()),
        ]),
        eject_line,
    ];
    for detail in &done.details {
        summary_lines.push(Line::from(Span::styled(detail.as_str(), theme::muted())));
    }
    if let Some(log_path) = &done.log_path {
        summary_lines.push(Line::from(vec![
            Span::styled("Log: ", theme::muted()),
            Span::raw(log_path.as_str()),
        ]));
    }

    frame.render_widget(
        Paragraph::new(summary_lines).block(ok_block).wrap(Wrap { trim: true }),
        chunks[0],
    );

    let steps = done.effective_next_steps();
    let step_lines: Vec<Line<'_>> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| {
            Line::from(vec![
                Span::styled(format!("{}. ", i + 1), theme::accent()),
                Span::raw(s.as_str()),
            ])
        })
        .collect();

    frame.render_widget(
        Paragraph::new(step_lines)
            .block(panel("Next Steps"))
            .wrap(Wrap { trim: true }),
        chunks[1],
    );
}

pub fn render_error(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let Some(err) = app.error_summary() else {
        return;
    };

    let has_logs = !app.progress_state().logs.is_empty();
    let (err_area, logs_area) = if has_logs && area.height >= 12 {
        let split = Layout::default()
            .direction(Direction::Vertical)
            .constraints([Constraint::Length(if compact { 7 } else { 9 }), Constraint::Min(4)])
            .split(area);
        (split[0], Some(split[1]))
    } else {
        (area, None)
    };

    let err_block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border_err())
        .title(Span::styled(format!(" {} ", err.title), theme::err_bold()));

    let mut lines = vec![Line::from(Span::styled(&err.message, theme::err_bold()))];
    if let Some(step) = &err.failed_step {
        lines.push(Line::from(vec![
            Span::styled("Failed step: ", theme::muted()),
            Span::raw(step.as_str()),
        ]));
    }

    let card_state_line = match &err.card_state {
        CardStateAfterError::Untouched => Line::from(vec![
            Span::styled("Card state: ", theme::muted()),
            Span::styled("Untouched — nothing was modified on the microSD card.", theme::ok()),
        ]),
        CardStateAfterError::Resumable { detail } => Line::from(vec![
            Span::styled("Card state: ", theme::muted()),
            Span::styled(format!("Resumable — {detail}"), theme::warn_bold()),
        ]),
        CardStateAfterError::Partial { detail } => Line::from(vec![
            Span::styled("Card state: ", theme::muted()),
            Span::styled(format!("Incomplete write — {detail}"), theme::err()),
        ]),
    };
    lines.push(card_state_line);

    if let Some(log_path) = &err.log_path {
        lines.push(Line::from(vec![
            Span::styled("Log file: ", theme::muted()),
            Span::styled(log_path.as_str(), theme::accent()),
        ]));
    }

    frame.render_widget(
        Paragraph::new(lines).block(err_block).wrap(Wrap { trim: true }),
        err_area,
    );

    if let Some(log_rect) = logs_area {
        render_logs(frame, log_rect, app.progress_state());
    }
}
