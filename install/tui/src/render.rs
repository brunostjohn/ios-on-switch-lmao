// SPDX-License-Identifier: GPL-3.0-or-later
use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

use crate::screens;
use crate::state::{App, Screen};
use crate::theme;

const MAX_CONTENT_WIDTH: u16 = 112;

pub fn render(frame: &mut Frame<'_>, app: &App) {
    let area = frame.area();
    if area.width < 10 || area.height < 6 {
        return;
    }

    let root = centered_area(area, MAX_CONTENT_WIDTH);
    let compact = root.height < 22 || root.width < 70;
    let header_h = if compact { 2 } else { 3 };
    let footer_h = if compact { 1 } else { 2 };

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(header_h),
            Constraint::Min(3),
            Constraint::Length(footer_h),
        ])
        .split(root);

    render_header(frame, chunks[0], app, compact);

    match app.screen() {
        Screen::PickMode => screens::wizard::render_pick_mode(frame, chunks[1], app, compact),
        Screen::PickBuild => screens::wizard::render_pick_build(frame, chunks[1], app, compact),
        Screen::PickCard => screens::wizard::render_pick_card(frame, chunks[1], app, compact),
        Screen::ResumeJournal => screens::wizard::render_resume(frame, chunks[1], app, compact),
        Screen::PickSize => screens::size_confirm::render_pick_size(frame, chunks[1], app, compact),
        Screen::Confirm => screens::size_confirm::render_confirm(frame, chunks[1], app, compact),
        Screen::Progress => screens::progress::render_progress(frame, chunks[1], app, compact),
        Screen::Done => screens::progress::render_done(frame, chunks[1], app, compact),
        Screen::Error => screens::progress::render_error(frame, chunks[1], app, compact),
    }

    render_footer(frame, chunks[2], app, compact);
}

fn centered_area(area: Rect, max_width: u16) -> Rect {
    if area.width <= max_width {
        return area;
    }
    let margin = (area.width - max_width) / 2;
    Rect {
        x: area.x + margin,
        y: area.y,
        width: max_width,
        height: area.height,
    }
}

fn render_header(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let crumbs = app.breadcrumbs();
    let mut crumb_spans: Vec<Span<'static>> = Vec::new();
    for (idx, crumb) in crumbs.iter().enumerate() {
        if idx > 0 {
            crumb_spans.push(Span::styled(" > ", theme::muted()));
        }
        let style = if crumb.active {
            theme::title()
        } else if crumb.done {
            theme::ok()
        } else {
            theme::muted()
        };
        crumb_spans.push(Span::styled(crumb.label, style));
    }

    let rule = "─".repeat(area.width as usize);
    let lines = if compact {
        let mut top = vec![Span::styled(app.title(), theme::title()), Span::raw("  ")];
        top.extend(crumb_spans);
        vec![Line::from(top), Line::from(Span::styled(rule, theme::border()))]
    } else {
        vec![
            Line::from(vec![
                Span::styled(app.title(), theme::title()),
                Span::raw("  "),
                Span::styled(app.mode().short_label(), theme::muted()),
            ]),
            Line::from(crumb_spans),
            Line::from(Span::styled(rule, theme::border())),
        ]
    };

    frame.render_widget(Paragraph::new(lines), area);
}

fn render_footer(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let hints = footer_hints(app, compact);
    let lines = if compact {
        vec![hints]
    } else {
        let rule = "─".repeat(area.width as usize);
        vec![Line::from(Span::styled(rule, theme::border())), hints]
    };
    frame.render_widget(Paragraph::new(lines), area);
}

fn footer_hints(app: &App, compact: bool) -> Line<'static> {
    let mut spans: Vec<Span<'static>> = Vec::new();
    let mut push = |key: &'static str, label: &'static str| {
        if !spans.is_empty() {
            spans.push(Span::raw("  "));
        }
        spans.push(Span::styled(key, theme::keycap()));
        spans.push(Span::raw(" "));
        spans.push(Span::styled(label, theme::muted()));
    };

    match app.screen() {
        Screen::PickMode => {
            push("↑/↓", "Move");
            push("1-3", "Pick");
            push("Enter", "Next");
            push("q", "Quit");
        }
        Screen::PickBuild => {
            push("↑/↓", "Move");
            push("Enter", "Select");
            if !app.mode_fixed() {
                push("Esc", "Back");
            }
            push("q", "Quit");
        }
        Screen::PickCard => {
            push("↑/↓", "Move");
            push("Enter", "Select");
            push("r", "Refresh");
            if app.mode().needs_build() || !app.mode_fixed() {
                push("Esc", "Back");
            }
            push("q", "Quit");
        }
        Screen::ResumeJournal => {
            push("↑/↓", "Choice");
            push("Enter/y", "Resume");
            push("Esc/n", "Back");
        }
        Screen::PickSize => {
            if compact {
                push("←/→", "±1G");
                push("PgUp/Dn", "±8G");
                push("[/]", "±16M");
                push("0-9", "GiB");
                push("Enter", "Next");
                push("Esc", "Back");
            } else {
                push("←/→", "±1 GiB");
                push("PgUp/PgDn", "±8 GiB");
                push("[/]", "±16 MiB");
                push("0-9", "Type GiB");
                push("Enter", "Next");
                push("Esc", "Back");
            }
        }
        Screen::Confirm => {
            push("Type ID", "Verify");
            push("Enter", "Start");
            push("Esc", "Back");
            push("Ctrl+C", "Quit");
        }
        Screen::Progress => {
            let p = app.progress_state();
            push("↑/↓", "Logs");
            if !p.follow_logs {
                push("End", "Follow");
            }
            if p.irreversible {
                spans.push(Span::raw("  "));
                spans.push(Span::styled("Do NOT unplug microSD card", theme::warn_bold()));
            } else if p.cancel_requested {
                spans.push(Span::raw("  "));
                spans.push(Span::styled("Cancelling...", theme::warn()));
            } else {
                push("q/Ctrl+C", "Cancel");
            }
        }
        Screen::Done | Screen::Error => {
            if !app.progress_state().logs.is_empty() {
                push("↑/↓", "Scroll logs");
            }
            push("Enter/q", "Exit");
        }
    }
    Line::from(spans)
}
