// SPDX-License-Identifier: GPL-3.0-or-later
//! Python's `int(text, base)`, for the integers the Python prelinkers read from their inputs.

/// `base` 0 takes the base from the prefix, as `int(text, 0)` does; 16 allows a `0x` prefix. Python's unbounded
/// integers are held to `i128`, which no prelink input comes near.
pub(crate) fn parse(text: &str, base: u32) -> Option<i128> {
    let text = text.trim();
    let (negative, text) = match text.as_bytes().first() {
        Some(b'-') => (true, &text[1..]),
        Some(b'+') => (false, &text[1..]),
        _ => (false, text),
    };
    let prefix = |p: &str| text.get(..2).is_some_and(|head| head.eq_ignore_ascii_case(p));
    let (radix, digits, prefixed) = match base {
        0 | 16 if prefix("0x") => (16, &text[2..], true),
        0 if prefix("0o") => (8, &text[2..], true),
        0 if prefix("0b") => (2, &text[2..], true),
        0 | 10 => (10, text, false),
        16 => (16, text, false),
        _ => return None,
    };
    let digits = if prefixed {
        digits.strip_prefix('_').unwrap_or(digits)
    } else {
        digits
    };
    if digits.is_empty() || digits.starts_with('_') || digits.ends_with('_') || digits.contains("__") {
        return None;
    }
    if base == 0 && radix == 10 && digits.starts_with('0') && digits.bytes().any(|b| b != b'0' && b != b'_') {
        return None;
    }
    let mut value: i128 = 0;
    for c in digits.chars().filter(|&c| c != '_') {
        value = value
            .checked_mul(i128::from(radix))?
            .checked_add(i128::from(c.to_digit(radix)?))?;
    }
    Some(if negative { -value } else { value })
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn matches_python() {
        let cases: &[(&str, u32, Option<i128>)] = &[
            ("0xfffffff005ab4ae0", 0, Some(0xffff_fff0_05ab_4ae0)),
            ("-0x1000000", 0, Some(-0x100_0000)),
            (" 42 ", 0, Some(42)),
            ("0", 0, Some(0)),
            ("00", 0, Some(0)),
            ("010", 0, None),
            ("0o17", 0, Some(15)),
            ("0b101", 0, Some(5)),
            ("0x_ff", 0, Some(255)),
            ("1_000", 0, Some(1000)),
            ("1__0", 0, None),
            ("_1", 0, None),
            ("1_", 0, None),
            ("0x", 0, None),
            ("", 0, None),
            ("710c011f", 16, Some(0x710c_011f)),
            ("0XFF", 16, Some(255)),
            ("0xfffffff005ebfd54", 16, Some(0xffff_fff0_05eb_fd54)),
            ("0x1", 10, None),
            ("12", 10, Some(12)),
            ("-1", 10, Some(-1)),
            ("g", 16, None),
        ];
        for &(text, base, expected) in cases {
            assert_eq!(parse(text, base), expected, "int({text:?}, {base})");
        }
    }
}
