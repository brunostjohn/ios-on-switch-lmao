// SPDX-License-Identifier: GPL-3.0-or-later
use ratatui::style::{Color, Modifier, Style};

pub fn title() -> Style {
    Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)
}

pub fn heading() -> Style {
    Style::default().add_modifier(Modifier::BOLD)
}

pub fn muted() -> Style {
    Style::default().fg(Color::DarkGray)
}

pub fn dimmed() -> Style {
    Style::default().fg(Color::DarkGray).add_modifier(Modifier::DIM)
}

pub fn border() -> Style {
    Style::default().fg(Color::DarkGray)
}

pub fn border_focused() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn border_ok() -> Style {
    Style::default().fg(Color::Green)
}

pub fn border_warn() -> Style {
    Style::default().fg(Color::Yellow)
}

pub fn border_err() -> Style {
    Style::default().fg(Color::Red)
}

pub fn selected_row() -> Style {
    Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)
}

pub fn ok() -> Style {
    Style::default().fg(Color::Green)
}

pub fn ok_bold() -> Style {
    Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)
}

pub fn warn() -> Style {
    Style::default().fg(Color::Yellow)
}

pub fn warn_bold() -> Style {
    Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
}

pub fn err() -> Style {
    Style::default().fg(Color::Red)
}

pub fn err_bold() -> Style {
    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)
}

pub fn accent() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn keycap() -> Style {
    Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)
}

pub fn bar_fat() -> Style {
    Style::default().fg(Color::Green)
}

pub fn bar_ios() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn bar_other() -> Style {
    Style::default().fg(Color::Yellow)
}
