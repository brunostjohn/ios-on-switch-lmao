// SPDX-License-Identifier: GPL-3.0-or-later
use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyModifiers};

use crate::state::{Action, Screen};

pub fn map_key(screen: Screen, key: KeyEvent) -> Option<Action> {
    if !matches!(key.kind, KeyEventKind::Press | KeyEventKind::Repeat) {
        return None;
    }

    if key.modifiers.contains(KeyModifiers::CONTROL) && matches!(key.code, KeyCode::Char('c' | 'C')) {
        return Some(Action::Quit);
    }

    match screen {
        Screen::PickMode => map_pick_mode(key),
        Screen::PickBuild => map_list_screen(key, false),
        Screen::PickCard => map_list_screen(key, true),
        Screen::ResumeJournal => map_resume(key),
        Screen::PickSize => map_pick_size(key),
        Screen::Confirm => map_confirm(key),
        Screen::Progress => map_progress(key),
        Screen::Done | Screen::Error => map_final(key),
    }
}

fn map_pick_mode(key: KeyEvent) -> Option<Action> {
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(Action::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(Action::Down),
        KeyCode::Char('1') => Some(Action::SelectIndex(0)),
        KeyCode::Char('2') => Some(Action::SelectIndex(1)),
        KeyCode::Char('3') => Some(Action::SelectIndex(2)),
        KeyCode::Enter if key.kind == KeyEventKind::Press => Some(Action::Confirm),
        KeyCode::Char('q') | KeyCode::Esc if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}

fn map_list_screen(key: KeyEvent, allow_refresh: bool) -> Option<Action> {
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(Action::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(Action::Down),
        KeyCode::Enter if key.kind == KeyEventKind::Press => Some(Action::Confirm),
        KeyCode::Esc | KeyCode::Backspace if key.kind == KeyEventKind::Press => Some(Action::Back),
        KeyCode::Char('r' | 'R') if allow_refresh && key.kind == KeyEventKind::Press => Some(Action::RefreshCards),
        KeyCode::Char('q') if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}

fn map_resume(key: KeyEvent) -> Option<Action> {
    match key.code {
        KeyCode::Up | KeyCode::Left | KeyCode::Char('k') => Some(Action::Up),
        KeyCode::Down | KeyCode::Right | KeyCode::Char('j') | KeyCode::Tab => Some(Action::Down),
        KeyCode::Enter if key.kind == KeyEventKind::Press => Some(Action::Confirm),
        KeyCode::Char('y' | 'Y') if key.kind == KeyEventKind::Press => Some(Action::SelectIndex(0)),
        KeyCode::Esc | KeyCode::Char('n' | 'N' | 'b' | 'B') if key.kind == KeyEventKind::Press => Some(Action::Back),
        KeyCode::Char('q') if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}

fn map_pick_size(key: KeyEvent) -> Option<Action> {
    if key.modifiers.contains(KeyModifiers::CONTROL) && matches!(key.code, KeyCode::Char('u' | 'U')) {
        return Some(Action::ClearInput);
    }
    match key.code {
        KeyCode::Left | KeyCode::Char('-') => Some(Action::Left),
        KeyCode::Right | KeyCode::Char('+' | '=') => Some(Action::Right),
        KeyCode::PageUp => Some(Action::PageUp),
        KeyCode::PageDown => Some(Action::PageDown),
        KeyCode::Char('[') => Some(Action::FineDecrease),
        KeyCode::Char(']') => Some(Action::FineIncrease),
        KeyCode::Home => Some(Action::Home),
        KeyCode::End => Some(Action::End),
        KeyCode::Backspace => Some(Action::Backspace),
        KeyCode::Char(c) if c.is_ascii_digit() || c == '.' => Some(Action::Char(c)),
        KeyCode::Enter if key.kind == KeyEventKind::Press => Some(Action::Confirm),
        KeyCode::Esc if key.kind == KeyEventKind::Press => Some(Action::Back),
        KeyCode::Char('q') if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}

fn map_confirm(key: KeyEvent) -> Option<Action> {
    if key.modifiers.contains(KeyModifiers::CONTROL) && matches!(key.code, KeyCode::Char('u' | 'U')) {
        return Some(Action::ClearInput);
    }
    match key.code {
        KeyCode::Enter if key.kind == KeyEventKind::Press => Some(Action::Confirm),
        KeyCode::Esc if key.kind == KeyEventKind::Press => Some(Action::Back),
        KeyCode::Backspace => Some(Action::Backspace),
        KeyCode::Char(c) if !key.modifiers.contains(KeyModifiers::CONTROL) => Some(Action::Char(c)),
        _ => None,
    }
}

fn map_progress(key: KeyEvent) -> Option<Action> {
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(Action::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(Action::Down),
        KeyCode::PageUp => Some(Action::PageUp),
        KeyCode::PageDown => Some(Action::PageDown),
        KeyCode::End | KeyCode::Char('G') => Some(Action::End),
        KeyCode::Home | KeyCode::Char('g') => Some(Action::Home),
        KeyCode::Char('q') if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}

fn map_final(key: KeyEvent) -> Option<Action> {
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(Action::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(Action::Down),
        KeyCode::PageUp => Some(Action::PageUp),
        KeyCode::PageDown => Some(Action::PageDown),
        KeyCode::End => Some(Action::End),
        KeyCode::Home => Some(Action::Home),
        KeyCode::Enter | KeyCode::Char('q') | KeyCode::Esc if key.kind == KeyEventKind::Press => Some(Action::Quit),
        _ => None,
    }
}
