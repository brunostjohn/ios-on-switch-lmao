// SPDX-License-Identifier: GPL-3.0-or-later

/// Python's `repr(float)`: the shortest digits that read back as `value` (which Rust's `{:e}` also gives),
/// positional for decimal exponents -4 to 15 and scientific with a signed two-digit exponent outside them.
pub fn repr(value: f64) -> String {
    if value.is_nan() {
        return "nan".into();
    }
    if value.is_infinite() {
        return if value < 0.0 { "-inf" } else { "inf" }.into();
    }
    let scientific = format!("{:e}", value.abs());
    let (mantissa, exponent) = scientific.split_once('e').expect("LowerExp always writes an exponent");
    let exponent: i32 = exponent.parse().expect("LowerExp writes a decimal exponent");
    let digits: String = mantissa.chars().filter(|c| *c != '.').collect();
    let sign = if value.is_sign_negative() { "-" } else { "" };
    if (-4..16).contains(&exponent) {
        format!("{sign}{}", positional(&digits, exponent))
    } else {
        let (first, rest) = digits.split_at(1);
        let fraction = if rest.is_empty() {
            String::new()
        } else {
            format!(".{rest}")
        };
        let exponent_sign = if exponent < 0 { '-' } else { '+' };
        format!("{sign}{first}{fraction}e{exponent_sign}{:02}", exponent.unsigned_abs())
    }
}

fn positional(digits: &str, exponent: i32) -> String {
    if exponent < 0 {
        return format!("0.{}{digits}", "0".repeat(exponent.unsigned_abs() as usize - 1));
    }
    let whole = exponent as usize + 1;
    if digits.len() <= whole {
        format!("{digits}{}.0", "0".repeat(whole - digits.len()))
    } else {
        format!("{}.{}", &digits[..whole], &digits[whole..])
    }
}
