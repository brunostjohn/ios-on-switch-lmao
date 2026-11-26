// SPDX-License-Identifier: GPL-3.0-or-later
pub mod progress;
pub mod size_confirm;
pub mod wizard;

use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Paragraph, Wrap};

use crate::theme;

pub fn panel(title: &str) -> Block<'_> {
    Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::border())
        .title(Span::styled(format!(" {title} "), theme::heading()))
}

pub fn render_notice(frame: &mut Frame<'_>, area: Rect, notice: Option<&str>) {
    let Some(msg) = notice else {
        return;
    };
    let style = if msg.starts_with("Cannot")
        || msg.starts_with("Not enough")
        || msg.starts_with("Card cannot")
        || msg.starts_with("Refresh failed")
        || msg.starts_with("No ")
        || msg.starts_with("Selected card has no")
    {
        theme::warn_bold()
    } else {
        theme::accent()
    };
    let line = Line::from(vec![Span::styled("• ", style), Span::styled(msg, style)]);
    frame.render_widget(Paragraph::new(line).wrap(Wrap { trim: true }), area);
}
