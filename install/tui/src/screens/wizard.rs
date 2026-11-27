// SPDX-License-Identifier: GPL-3.0-or-later
use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, List, ListItem, ListState, Paragraph, Wrap};

use crate::model::{Mode, SlotStatus, format_bytes};
use crate::screens::{panel, render_notice};
use crate::state::App;
use crate::theme;

pub fn render_pick_mode(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(if compact { 1 } else { 2 }),
            Constraint::Min(6),
            Constraint::Length(if app.notice().is_some() { 1 } else { 0 }),
        ])
        .split(area);

    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(
            "Choose what to do with your Switch microSD card:",
            theme::heading(),
        ))),
        chunks[0],
    );

    let items: Vec<ListItem<'_>> = Mode::ALL
        .iter()
        .enumerate()
        .map(|(i, mode)| {
            let selected = i == app.mode_index();
            let marker = if selected { "▸ " } else { "  " };
            let title_style = if selected {
                theme::selected_row()
            } else {
                theme::heading()
            };
            let line1 = Line::from(vec![
                Span::styled(marker, title_style),
                Span::styled(format!("{}. {}", i + 1, mode.label()), title_style),
            ]);
            if compact {
                ListItem::new(vec![
                    line1,
                    Line::from(vec![
                        Span::raw("     "),
                        Span::styled(mode.description(), theme::muted()),
                    ]),
                ])
            } else {
                ListItem::new(vec![
                    line1,
                    Line::from(vec![
                        Span::raw("     "),
                        Span::styled(mode.description(), theme::muted()),
                    ]),
                    Line::raw(""),
                ])
            }
        })
        .collect();

    let mut list_state = ListState::default();
    list_state.select(Some(app.mode_index()));
    let list = List::new(items).block(panel("Mode"));
    frame.render_stateful_widget(list, chunks[1], &mut list_state);

    render_notice(frame, chunks[2], app.notice());
}

pub fn render_pick_build(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let detail_h = if compact { 3 } else { 5 };
    let notice_h = if app.notice().is_some() { 1 } else { 0 };
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(5),
            Constraint::Length(detail_h),
            Constraint::Length(notice_h),
        ])
        .split(area);

    let builds = app.builds();
    if builds.is_empty() {
        let empty = Paragraph::new(vec![
            Line::from(Span::styled(
                "No builds found in the release catalog.",
                theme::warn_bold(),
            )),
            Line::from(Span::styled(
                "Check your network connection or catalog source and try again.",
                theme::muted(),
            )),
        ])
        .block(panel("Select iPadOS Build"))
        .wrap(Wrap { trim: true });
        frame.render_widget(empty, chunks[0]);
        render_notice(frame, chunks[2], app.notice());
        return;
    }

    let items: Vec<ListItem<'_>> = builds
        .iter()
        .enumerate()
        .map(|(i, build)| {
            let selected = i == app.build_index();
            let marker = if selected { "▸ " } else { "  " };
            if build.is_supported() {
                let label_style = if selected {
                    theme::selected_row()
                } else {
                    theme::heading()
                };
                let meta = format!("{} · {} · {}", build.product, build.release_tag, build.size_summary);
                let mut lines = vec![Line::from(vec![
                    Span::styled(marker, label_style),
                    Span::styled(&build.label, label_style),
                ])];
                if !compact {
                    lines.push(Line::from(vec![Span::raw("  "), Span::styled(meta, theme::muted())]));
                }
                ListItem::new(lines)
            } else {
                let style = theme::dimmed();
                let marker_style = if selected { theme::warn() } else { style };
                let mut lines = vec![Line::from(vec![
                    Span::styled(marker, marker_style),
                    Span::styled(&build.label, style),
                    Span::raw(" "),
                    Span::styled("[Unsupported]", theme::warn()),
                ])];
                if !compact && let Some(reason) = &build.unsupported_reason {
                    lines.push(Line::from(vec![Span::raw("  "), Span::styled(reason.as_str(), style)]));
                }
                ListItem::new(lines)
            }
        })
        .collect();

    let mut state = ListState::default();
    state.select(Some(app.build_index()));
    frame.render_stateful_widget(
        List::new(items).block(panel("Select iPadOS Build")),
        chunks[0],
        &mut state,
    );

    if let Some(build) = builds.get(app.build_index()) {
        let mut lines = vec![Line::from(vec![
            Span::styled(&build.label, theme::heading()),
            Span::raw("  "),
            Span::styled(format!("{} · {}", build.product, build.release_tag), theme::accent()),
        ])];
        if let Some(reason) = &build.unsupported_reason {
            lines.push(Line::from(vec![
                Span::styled("Unsupported: ", theme::warn_bold()),
                Span::styled(reason.as_str(), theme::warn()),
            ]));
        } else {
            lines.push(Line::from(Span::styled(&build.size_summary, theme::muted())));
        }
        frame.render_widget(
            Paragraph::new(lines).block(panel("Details")).wrap(Wrap { trim: true }),
            chunks[1],
        );
    }

    render_notice(frame, chunks[2], app.notice());
}

pub fn render_pick_card(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let cards = app.cards();
    let notice_h = if app.notice().is_some() { 1 } else { 0 };

    if cards.is_empty() {
        let chunks = Layout::default()
            .direction(Direction::Vertical)
            .constraints([Constraint::Min(6), Constraint::Length(notice_h)])
            .split(area);
        let warn_block = Block::default()
            .borders(Borders::ALL)
            .border_type(BorderType::Rounded)
            .border_style(theme::border_warn())
            .title(Span::styled(" Select microSD Card ", theme::warn_bold()));
        let text = vec![
            Line::from(Span::styled("No removable microSD cards detected.", theme::warn_bold())),
            Line::raw(""),
            Line::from(Span::styled(
                "Insert your Nintendo Switch microSD card into a card reader, then press 'r' to refresh.",
                theme::muted(),
            )),
            Line::from(Span::styled(
                "Internal and system disks are hidden for safety.",
                theme::muted(),
            )),
        ];
        frame.render_widget(
            Paragraph::new(text).block(warn_block).wrap(Wrap { trim: true }),
            chunks[0],
        );
        render_notice(frame, chunks[1], app.notice());
        return;
    }

    let detail_h = if compact { 3 } else { 5 };
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(5),
            Constraint::Length(detail_h),
            Constraint::Length(notice_h),
        ])
        .split(area);

    let items: Vec<ListItem<'_>> = cards
        .iter()
        .enumerate()
        .map(|(i, card)| {
            let selected = i == app.card_index();
            let marker = if selected { "▸ " } else { "  " };
            let unusable = card.pending_journal.is_none() && card.unusable_reason.is_some();
            let main_style = if unusable {
                theme::dimmed()
            } else if selected {
                theme::selected_row()
            } else {
                theme::heading()
            };

            let (badge_text, badge_style) = if card.pending_journal.is_some() {
                ("[RESUME REQUIRED]".to_owned(), theme::warn_bold())
            } else if unusable {
                ("[Unavailable]".to_owned(), theme::warn())
            } else {
                let style = match card.slot {
                    SlotStatus::ExistingIos { .. } => theme::ok(),
                    SlotStatus::ExistingSetup { .. } => theme::warn(),
                    SlotStatus::FreshCandidate { .. } => theme::accent(),
                    SlotStatus::None => theme::muted(),
                };
                (format!("[{}]", card.slot.badge()), style)
            };

            let line1 = Line::from(vec![
                Span::styled(marker, if selected { theme::accent() } else { main_style }),
                Span::styled(&card.id, main_style),
                Span::raw("  "),
                Span::styled(&card.name, main_style),
                Span::raw("  "),
                Span::styled(format_bytes(card.capacity_bytes), theme::muted()),
                Span::raw("  "),
                Span::styled(badge_text, badge_style),
            ]);

            if compact {
                ListItem::new(vec![line1])
            } else {
                ListItem::new(vec![
                    line1,
                    Line::from(vec![
                        Span::raw("  "),
                        Span::styled(&card.layout_summary, theme::muted()),
                    ]),
                ])
            }
        })
        .collect();

    let mut state = ListState::default();
    state.select(Some(app.card_index()));
    frame.render_stateful_widget(
        List::new(items).block(panel("Select microSD Card")),
        chunks[0],
        &mut state,
    );

    if let Some(card) = app.selected_card() {
        let mut lines = vec![Line::from(vec![
            Span::styled(&card.id, theme::heading()),
            Span::raw(" — "),
            Span::styled(&card.layout_summary, theme::muted()),
        ])];
        if let Some(journal) = &card.pending_journal {
            lines.push(Line::from(vec![
                Span::styled("Pending partition journal: ", theme::warn_bold()),
                Span::styled(&journal.stage, theme::warn()),
            ]));
        } else if let Some(reason) = &card.unusable_reason {
            lines.push(Line::from(vec![
                Span::styled("Unavailable: ", theme::warn_bold()),
                Span::styled(reason.as_str(), theme::warn()),
            ]));
        } else {
            lines.push(Line::from(vec![
                Span::styled("Slot: ", theme::muted()),
                Span::styled(card.slot.badge(), theme::accent()),
            ]));
        }
        frame.render_widget(
            Paragraph::new(lines)
                .block(panel("Card Details"))
                .wrap(Wrap { trim: true }),
            chunks[1],
        );
    }

    render_notice(frame, chunks[2], app.notice());
}

pub fn render_resume(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let Some(card) = app.selected_card() else {
        return;
    };
    let Some(journal) = &card.pending_journal else {
        return;
    };

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Min(7), Constraint::Length(if compact { 4 } else { 5 })])
        .split(area);

    let warn_block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border_warn())
        .title(Span::styled(" Resume Interrupted Partition ", theme::warn_bold()));

    let mut lines = vec![
        Line::from(vec![
            Span::styled("Card: ", theme::muted()),
            Span::styled(
                format!("{} — {} ({})", card.id, card.name, format_bytes(card.capacity_bytes)),
                theme::heading(),
            ),
        ]),
        Line::from(vec![
            Span::styled("Interrupted stage: ", theme::muted()),
            Span::styled(&journal.stage, theme::warn_bold()),
        ]),
    ];
    if !journal.detail.is_empty() {
        lines.push(Line::from(vec![
            Span::styled("Detail: ", theme::muted()),
            Span::raw(&journal.detail),
        ]));
    }
    if !compact {
        lines.push(Line::raw(""));
    }
    lines.push(Line::from(Span::styled(
        "A previous partition run stopped before finishing. Resume it first so FAT32 and the partition tables finish cleanly before starting any new operation.",
        theme::muted(),
    )));

    frame.render_widget(
        Paragraph::new(lines).block(warn_block).wrap(Wrap { trim: true }),
        chunks[0],
    );

    let sel = app.resume_state().selected;
    let choice_lines = vec![
        Line::from(vec![
            Span::styled(if sel == 0 { "▸ " } else { "  " }, theme::ok_bold()),
            Span::styled(
                "Resume and finish interrupted partition",
                if sel == 0 { theme::ok_bold() } else { theme::heading() },
            ),
        ]),
        Line::from(vec![
            Span::styled(if sel == 1 { "▸ " } else { "  " }, theme::accent()),
            Span::styled(
                "Go back to card list",
                if sel == 1 {
                    theme::selected_row()
                } else {
                    theme::muted()
                },
            ),
        ]),
    ];
    frame.render_widget(Paragraph::new(choice_lines).block(panel("Action")), chunks[1]);
}
