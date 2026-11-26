// SPDX-License-Identifier: GPL-3.0-or-later
use crate::model::{ALIGN_BYTES, DEFAULT_IOS_BYTES, SizeConstraints, align_down, align_up, format_gib, snap_nearest};

const GIB: u128 = 1024 * 1024 * 1024;
const MAX_INPUT_LEN: usize = 12;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SizeState {
    pub constraints: SizeConstraints,
    pub min_bytes: u64,
    pub max_bytes: u64,
    pub selected_bytes: u64,
    pub impossible: bool,
    pub input_buf: String,
    pub input_note: Option<String>,
    pre_input_bytes: u64,
}

impl SizeState {
    pub fn new(constraints: SizeConstraints, build_min_bytes: Option<u64>) -> Self {
        let min_raw = constraints.min_bytes.max(build_min_bytes.unwrap_or(0));
        let min_bytes = align_up(min_raw, ALIGN_BYTES);
        let max_bytes = align_down(constraints.max_bytes, ALIGN_BYTES);
        let impossible = min_bytes > max_bytes;
        let selected_bytes = if impossible {
            max_bytes
        } else {
            let target = constraints.default_bytes.unwrap_or(DEFAULT_IOS_BYTES);
            snap_nearest(target, ALIGN_BYTES).clamp(min_bytes, max_bytes)
        };
        Self {
            constraints,
            min_bytes,
            max_bytes,
            selected_bytes,
            impossible,
            input_buf: String::new(),
            input_note: None,
            pre_input_bytes: selected_bytes,
        }
    }

    pub fn step_by(&mut self, delta: i64) {
        if self.impossible {
            return;
        }
        self.input_buf.clear();
        self.input_note = None;
        let raw = if delta >= 0 {
            self.selected_bytes.saturating_add(delta as u64)
        } else {
            self.selected_bytes.saturating_sub(delta.unsigned_abs())
        };
        self.selected_bytes = snap_nearest(raw, ALIGN_BYTES).clamp(self.min_bytes, self.max_bytes);
        self.pre_input_bytes = self.selected_bytes;
    }

    pub fn set_to_min(&mut self) {
        if self.impossible {
            return;
        }
        self.input_buf.clear();
        self.input_note = None;
        self.selected_bytes = self.min_bytes;
        self.pre_input_bytes = self.selected_bytes;
    }

    pub fn set_to_max(&mut self) {
        if self.impossible {
            return;
        }
        self.input_buf.clear();
        self.input_note = None;
        self.selected_bytes = self.max_bytes;
        self.pre_input_bytes = self.selected_bytes;
    }

    pub fn push_input_char(&mut self, c: char) {
        if self.impossible || self.input_buf.len() >= MAX_INPUT_LEN {
            return;
        }
        if c == '.' && self.input_buf.contains('.') {
            return;
        }
        if !c.is_ascii_digit() && c != '.' {
            return;
        }
        if self.input_buf.is_empty() {
            self.pre_input_bytes = self.selected_bytes;
        }
        self.input_buf.push(c);
        self.apply_input_buf();
    }

    pub fn pop_input_char(&mut self) {
        if self.impossible || self.input_buf.is_empty() {
            return;
        }
        self.input_buf.pop();
        if self.input_buf.is_empty() {
            self.selected_bytes = self.pre_input_bytes;
            self.input_note = None;
        } else {
            self.apply_input_buf();
        }
    }

    pub fn clear_input(&mut self) {
        if self.input_buf.is_empty() {
            return;
        }
        self.input_buf.clear();
        self.selected_bytes = self.pre_input_bytes;
        self.input_note = None;
    }

    pub fn remaining_fat_bytes(&self) -> u64 {
        self.constraints.fat_total_bytes.saturating_sub(self.selected_bytes)
    }

    pub fn remaining_fat_free_bytes(&self) -> u64 {
        self.remaining_fat_bytes()
            .saturating_sub(self.constraints.fat_used_bytes)
    }

    pub fn estimated_relocate_bytes(&self) -> u64 {
        self.constraints.estimate_relocation(self.selected_bytes)
    }

    fn apply_input_buf(&mut self) {
        let Some(raw) = parse_gib_to_bytes(&self.input_buf) else {
            self.input_note = None;
            return;
        };
        let snapped = snap_nearest(raw, ALIGN_BYTES);
        let clamped = snapped.clamp(self.min_bytes, self.max_bytes);
        self.selected_bytes = clamped;
        if raw < self.min_bytes {
            self.input_note = Some(format!("Clamped to minimum ({})", format_gib(self.min_bytes)));
        } else if raw > self.max_bytes {
            self.input_note = Some(format!("Clamped to maximum ({})", format_gib(self.max_bytes)));
        } else if snapped != raw {
            self.input_note = Some(format!("Snapped to 16 MiB alignment ({})", format_gib(clamped)));
        } else {
            self.input_note = None;
        }
    }
}

/// Parses a decimal GiB string (such as `"64"` or `"32.5"`) into bytes without floating-point drift.
pub fn parse_gib_to_bytes(text: &str) -> Option<u64> {
    let trimmed = text.trim();
    if trimmed.is_empty() || trimmed == "." {
        return None;
    }
    let mut parts = trimmed.split('.');
    let whole_str = parts.next()?;
    let frac_str = parts.next();
    if parts.next().is_some() {
        return None;
    }

    let whole: u128 = if whole_str.is_empty() {
        0
    } else {
        if !whole_str.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        whole_str.parse::<u128>().unwrap_or(u128::MAX)
    };

    let mut total = whole.saturating_mul(GIB);

    if let Some(frac) = frac_str
        && !frac.is_empty()
    {
        if !frac.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let capped = &frac[..frac.len().min(9)];
        let frac_val: u128 = capped.parse().ok()?;
        let scale = 10u128.pow(capped.len() as u32);
        let frac_bytes = (frac_val.saturating_mul(GIB) + scale / 2) / scale;
        total = total.saturating_add(frac_bytes);
    }

    Some(total.min(u64::MAX as u128) as u64)
}
