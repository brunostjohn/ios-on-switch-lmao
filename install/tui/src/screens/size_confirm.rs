// SPDX-License-Identifier: GPL-3.0-or-later
use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Paragraph, Wrap};

use crate::model::{CardOption, Mode, format_bytes, format_gib};
use crate::screens::{panel, render_notice};
use crate::size_state::SizeState;
use crate::state::App;
use crate::theme;

pub fn render_pick_size(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let Some(size) = app.size_state() else {
        return;
    };

    if size.impossible {
        render_size_impossible(frame, area, size);
        return;
    }

    let notice = app.notice().or(size.input_note.as_deref());
    let notice_h = if notice.is_some() { 1 } else { 0 };
    let bar_h = if compact { 4 } else { 5 };
    let input_h = if compact { 4 } else { 5 };

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(bar_h),
            Constraint::Length(input_h),
            Constraint::Min(3),
            Constraint::Length(notice_h),
        ])
        .split(area);

    render_partition_bar(frame, chunks[0], size, compact);
    render_size_input(frame, chunks[1], size, compact);
    render_size_details(frame, chunks[2], size, compact);
    render_notice(frame, chunks[3], notice);
}

fn render_size_impossible(frame: &mut Frame<'_>, area: Rect, size: &SizeState) {
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border_err())
        .title(Span::styled(" Insufficient Free Space ", theme::err_bold()));
    let lines = vec![
        Line::from(Span::styled(
            "This microSD card does not have enough free space in FAT32 to carve an iOS partition.",
            theme::err_bold(),
        )),
        Line::raw(""),
        Line::from(vec![
            Span::styled("Minimum required: ", theme::muted()),
            Span::styled(format_gib(size.min_bytes), theme::heading()),
        ]),
        Line::from(vec![
            Span::styled("Maximum carvable: ", theme::muted()),
            Span::styled(format_gib(size.max_bytes), theme::warn_bold()),
        ]),
        Line::raw(""),
        Line::from(Span::styled(
            "Free up space on the Horizon FAT32 volume or press Esc to choose another card.",
            theme::muted(),
        )),
    ];
    frame.render_widget(Paragraph::new(lines).block(block).wrap(Wrap { trim: true }), area);
}

fn render_partition_bar(frame: &mut Frame<'_>, area: Rect, size: &SizeState, compact: bool) {
    let inner_width = area.width.saturating_sub(2) as usize;
    let remaining_fat = size.remaining_fat_bytes();
    let ios = size.selected_bytes;
    let other = size.constraints.other_partitions_bytes;
    let total = size
        .constraints
        .total_card_bytes
        .max(remaining_fat.saturating_add(ios).saturating_add(other))
        .max(1);

    let (fat_cols, ios_cols, other_cols) = segment_widths(inner_width, remaining_fat, ios, other, total);
    let bar_line = Line::from(vec![
        Span::styled("█".repeat(fat_cols), theme::bar_fat()),
        Span::styled("▓".repeat(ios_cols), theme::bar_ios()),
        Span::styled("░".repeat(other_cols), theme::bar_other()),
    ]);

    let mut lines = vec![bar_line];
    if !compact {
        lines.push(Line::raw(""));
    }
    let mut legend = vec![
        Span::styled("█ ", theme::bar_fat()),
        Span::raw(format!("FAT32 {}  ", format_bytes(remaining_fat))),
        Span::styled("▓ ", theme::bar_ios()),
        Span::raw(format!("iOS {}  ", format_bytes(ios))),
    ];
    if other > 0 {
        legend.push(Span::styled("░ ", theme::bar_other()));
        legend.push(Span::raw(format!("Other (L4T/emuMMC) {}", format_bytes(other))));
    }
    lines.push(Line::from(legend));

    frame.render_widget(Paragraph::new(lines).block(panel("Card Layout Preview")), area);
}

fn render_size_input(frame: &mut Frame<'_>, area: Rect, size: &SizeState, compact: bool) {
    let entry_display = if size.input_buf.is_empty() {
        Span::styled("type GiB (e.g. 64 or 48.5)", theme::muted())
    } else {
        Span::styled(format!("{} GiB_", size.input_buf), theme::selected_row())
    };

    let mut lines = vec![Line::from(vec![
        Span::styled("iOS partition size: ", theme::muted()),
        Span::styled(format_gib(size.selected_bytes), theme::title()),
        Span::styled("  (16 MiB aligned)", theme::muted()),
    ])];
    lines.push(Line::from(vec![
        Span::styled("Direct entry: ", theme::muted()),
        entry_display,
        Span::raw("   "),
        Span::styled(
            format!(
                "Range: {} .. {}",
                format_gib(size.min_bytes),
                format_gib(size.max_bytes)
            ),
            theme::muted(),
        ),
    ]));
    if !compact {
        lines.push(Line::from(Span::styled(
            "Use ←/→ (±1 GiB), PgUp/PgDn (±8 GiB), [/] (±16 MiB), or type a GiB value.",
            theme::muted(),
        )));
    }

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border_focused())
        .title(Span::styled(" Choose iOS Partition Size ", theme::title()));
    frame.render_widget(Paragraph::new(lines).block(block).wrap(Wrap { trim: true }), area);
}

fn render_size_details(frame: &mut Frame<'_>, area: Rect, size: &SizeState, compact: bool) {
    let relocate = size.estimated_relocate_bytes();
    let relocate_str = if relocate == 0 {
        "0 B (no tail clusters to move)".to_owned()
    } else {
        format!("~{} moved from FAT32 tail", format_bytes(relocate))
    };

    let mut lines = vec![
        Line::from(vec![
            Span::styled("Remaining Horizon FAT32: ", theme::muted()),
            Span::styled(format_bytes(size.remaining_fat_bytes()), theme::ok()),
            Span::styled(
                format!(
                    " ({} used, {} free)",
                    format_bytes(size.constraints.fat_used_bytes),
                    format_bytes(size.remaining_fat_free_bytes())
                ),
                theme::muted(),
            ),
        ]),
        Line::from(vec![
            Span::styled("Estimated FAT tail relocation: ", theme::muted()),
            Span::styled(relocate_str, if relocate > 0 { theme::warn() } else { theme::ok() }),
        ]),
    ];
    if !compact && size.constraints.other_partitions_bytes > 0 {
        lines.push(Line::from(vec![
            Span::styled("Other partitions (L4T / emuMMC): ", theme::muted()),
            Span::styled(
                format!("{} (untouched)", format_bytes(size.constraints.other_partitions_bytes)),
                theme::ok(),
            ),
        ]));
    }
    frame.render_widget(
        Paragraph::new(lines).block(panel("Impact")).wrap(Wrap { trim: true }),
        area,
    );
}

pub fn render_confirm(frame: &mut Frame<'_>, area: Rect, app: &App, compact: bool) {
    let Some(card) = app.selected_card() else {
        return;
    };

    let summary_h = if compact { 5 } else { 7 };
    let input_h = if app.confirm_state().error.is_some() { 4 } else { 3 };

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(summary_h),
            Constraint::Min(4),
            Constraint::Length(input_h),
        ])
        .split(area);

    render_confirm_summary(frame, chunks[0], app, card, compact);
    render_confirm_impact(frame, chunks[1], app, card, compact);
    render_confirm_input(frame, chunks[2], app, card);
}

fn render_confirm_summary(frame: &mut Frame<'_>, area: Rect, app: &App, card: &CardOption, compact: bool) {
    let mut lines = vec![Line::from(vec![
        Span::styled("Mode: ", theme::muted()),
        Span::styled(app.mode().label(), theme::heading()),
        Span::raw("   "),
        Span::styled("Target: ", theme::muted()),
        Span::styled(
            format!("{} — {} ({})", card.id, card.name, format_bytes(card.capacity_bytes)),
            theme::warn_bold(),
        ),
    ])];

    if let Some(build) = app.selected_build() {
        lines.push(Line::from(vec![
            Span::styled("Build: ", theme::muted()),
            Span::styled(&build.label, theme::accent()),
            Span::styled(format!(" ({} · {})", build.product, build.release_tag), theme::muted()),
        ]));
    }

    lines.push(Line::from(vec![
        Span::styled("Before: ", theme::muted()),
        Span::raw(&card.layout_summary),
    ]));

    let after = build_after_summary(app, card);
    lines.push(Line::from(vec![
        Span::styled("After:  ", theme::muted()),
        Span::styled(after, theme::accent()),
    ]));

    if compact {
        lines.truncate(area.height.saturating_sub(2) as usize);
    }

    frame.render_widget(
        Paragraph::new(lines)
            .block(panel("Confirm Plan"))
            .wrap(Wrap { trim: true }),
        area,
    );
}

fn render_confirm_impact(frame: &mut Frame<'_>, area: Rect, app: &App, card: &CardOption, compact: bool) {
    let preserved = effective_preserved(card);
    let overwritten = effective_overwritten(app, card);

    if area.width >= 72 && !compact {
        let cols = Layout::default()
            .direction(Direction::Horizontal)
            .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
            .split(area);
        let p_lines: Vec<Line<'_>> = preserved
            .iter()
            .map(|item| Line::from(vec![Span::styled("✓ ", theme::ok_bold()), Span::raw(item.as_str())]))
            .collect();
        let o_lines: Vec<Line<'_>> = overwritten
            .iter()
            .map(|item| Line::from(vec![Span::styled("! ", theme::warn_bold()), Span::raw(item.as_str())]))
            .collect();
        frame.render_widget(
            Paragraph::new(p_lines)
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .border_type(BorderType::Rounded)
                        .border_style(theme::border_ok())
                        .title(Span::styled(" Preserved ", theme::ok_bold())),
                )
                .wrap(Wrap { trim: true }),
            cols[0],
        );
        frame.render_widget(
            Paragraph::new(o_lines)
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .border_type(BorderType::Rounded)
                        .border_style(theme::border_warn())
                        .title(Span::styled(" Overwritten / Modified ", theme::warn_bold())),
                )
                .wrap(Wrap { trim: true }),
            cols[1],
        );
    } else {
        let lines = vec![
            Line::from(vec![
                Span::styled("✓ Preserved: ", theme::ok_bold()),
                Span::raw(preserved.join(", ")),
            ]),
            Line::from(vec![
                Span::styled("! Modified:  ", theme::warn_bold()),
                Span::raw(overwritten.join(", ")),
            ]),
        ];
        frame.render_widget(
            Paragraph::new(lines)
                .block(panel("Preserved vs Modified"))
                .wrap(Wrap { trim: true }),
            area,
        );
    }
}

fn render_confirm_input(frame: &mut Frame<'_>, area: Rect, app: &App, card: &CardOption) {
    let confirm = app.confirm_state();
    let expected = card.effective_confirm_token();
    let matched = card.matches_confirm(&confirm.typed);

    let (border_style, title_style) = if confirm.error.is_some() {
        (theme::border_err(), theme::err_bold())
    } else if matched {
        (theme::border_ok(), theme::ok_bold())
    } else {
        (theme::border_focused(), theme::warn_bold())
    };

    let case_hint = if card.confirm_case_sensitive {
        ""
    } else {
        " (case-insensitive)"
    };
    let title = format!(" Type \"{expected}\"{case_hint} and press Enter ");

    let mut lines = vec![Line::from(vec![
        Span::styled("> ", theme::accent()),
        Span::styled(
            &confirm.typed,
            if matched { theme::ok_bold() } else { theme::heading() },
        ),
        Span::styled("_", theme::accent()),
    ])];
    if let Some(err) = &confirm.error {
        lines.push(Line::from(Span::styled(err.as_str(), theme::err_bold())));
    }

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(border_style)
        .title(Span::styled(title, title_style));
    frame.render_widget(Paragraph::new(lines).block(block), area);
}

fn build_after_summary(app: &App, card: &CardOption) -> String {
    match app.mode() {
        Mode::Install => format!(
            "Target slot ({}) written with iPadOS APFS container and marked 'iOS'; any older root demoted to 'userdata'.",
            card.slot.badge()
        ),
        Mode::Partition => {
            if let Some(size) = app.size_state() {
                format!(
                    "FAT32 shrunk to {} (~{} relocated); new {} 'iOS-setup' partition carved.",
                    format_bytes(size.remaining_fat_bytes()),
                    format_bytes(size.estimated_relocate_bytes()),
                    format_gib(size.selected_bytes),
                )
            } else {
                "FAT32 shrunk and new 'iOS-setup' partition carved.".to_owned()
            }
        }
        Mode::PartitionAndInstall => {
            if let Some(size) = app.size_state() {
                format!(
                    "FAT32 shrunk to {} (~{} relocated); new {} 'iOS' partition carved and installed.",
                    format_bytes(size.remaining_fat_bytes()),
                    format_bytes(size.estimated_relocate_bytes()),
                    format_gib(size.selected_bytes),
                )
            } else {
                "FAT32 shrunk, new 'iOS' partition carved, and iPadOS installed.".to_owned()
            }
        }
    }
}

fn effective_preserved(card: &CardOption) -> Vec<String> {
    if !card.preserved_items.is_empty() {
        return card.preserved_items.clone();
    }
    vec![
        "Horizon OS games & saves".to_owned(),
        "emuMMC & L4T partitions".to_owned(),
        "switchroot/ios/bootargs.txt".to_owned(),
    ]
}

fn effective_overwritten(app: &App, card: &CardOption) -> Vec<String> {
    if !card.overwritten_items.is_empty() {
        return card.overwritten_items.clone();
    }
    match app.mode() {
        Mode::Install => vec![
            format!("Target partition ({}) wiped and formatted as APFS", card.slot.badge()),
            "Boot files: switchroot/ios/*, bootloader/ini/ios.ini, icon".to_owned(),
        ],
        Mode::Partition => vec![
            "FAT32 tail clusters relocated & volume shrunk".to_owned(),
            "GPT + hybrid MBR updated with new 'iOS-setup' entry".to_owned(),
        ],
        Mode::PartitionAndInstall => vec![
            "FAT32 tail relocated, GPT + hybrid MBR updated".to_owned(),
            "New iOS partition formatted as APFS; switchroot/ios/* & ios.ini updated".to_owned(),
        ],
    }
}

fn segment_widths(width: usize, fat: u64, ios: u64, other: u64, total: u64) -> (usize, usize, usize) {
    if width == 0 || total == 0 {
        return (0, 0, 0);
    }
    let mut ios_w = ((ios as u128 * width as u128) / total as u128) as usize;
    let mut other_w = ((other as u128 * width as u128) / total as u128) as usize;
    if ios > 0 && ios_w == 0 && width >= 3 {
        ios_w = 1;
    }
    if other > 0 && other_w == 0 && width >= 3 {
        other_w = 1;
    }
    let used = ios_w.saturating_add(other_w);
    if used >= width {
        let ios_clamped = ios_w.min(width);
        let other_clamped = width.saturating_sub(ios_clamped);
        return (0, ios_clamped, other_clamped);
    }
    let fat_w = if fat > 0 { width - used } else { 0 };
    (fat_w, ios_w, other_w)
}
