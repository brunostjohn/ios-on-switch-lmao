// SPDX-License-Identifier: GPL-3.0-or-later
//! Dates as plistlib holds them: a naive `datetime`, whole microseconds after 2001-01-01, read through
//! `datetime(2001, 1, 1) + timedelta(seconds=f)` and written as `(value - datetime(2001, 1, 1)).total_seconds()`.

/// Microseconds after 2001-01-01T00:00:00.
pub type Micros = i64;

const US_PER_SECOND: i128 = 1_000_000;
const US_PER_DAY: i128 = 86_400 * US_PER_SECOND;
// date(2001, 1, 1).toordinal() less date(1, 1, 1)'s, and date(9999, 12, 31)'s less date(2001, 1, 1)'s.
const DAYS_BEFORE_2001: i128 = 730_485;
const DAYS_AFTER_2001: i128 = 2_921_573;
const MIN: i128 = -DAYS_BEFORE_2001 * US_PER_DAY;
const MAX: i128 = (DAYS_AFTER_2001 + 1) * US_PER_DAY - 1;
// Far past the datetime range on either side, so the integer part can't overflow before the range check.
const SECONDS_BOUND: f64 = 1e15;

/// CPython's `timedelta(seconds=f)` (`delta_new` and `accum`): the integer part exactly, the fraction's microseconds
/// through one float product, and what is left rounded half to even on the total. `None` where plistlib raises.
pub fn from_seconds(seconds: f64) -> Option<Micros> {
    if !seconds.is_finite() || seconds.abs() >= SECONDS_BOUND {
        return None;
    }
    let whole = seconds.trunc();
    let fraction = seconds - whole;
    let mut total = whole as i128 * US_PER_SECOND;
    if fraction != 0.0 {
        let product = 1e6 * fraction;
        let product_whole = product.trunc();
        total += product_whole as i128;
        let leftover = product - product_whole;
        if leftover != 0.0 {
            let mut rounded = leftover.round();
            if (rounded - leftover).abs() == 0.5 {
                let odd = (total & 1) as f64;
                rounded = 2.0 * ((leftover + odd) * 0.5).round() - odd;
            }
            total += rounded as i128;
        }
    }
    (MIN..=MAX).contains(&total).then_some(total as Micros)
}

/// `timedelta.total_seconds()`: the microseconds over 10⁶, correctly rounded as Python's integer true division is.
pub fn to_seconds(micros: Micros) -> f64 {
    if micros == 0 {
        return 0.0;
    }
    // 10⁶ = 15625 · 2⁶. Scale the numerator until the quotient by 15625 has 54 bits, then round to 53.
    const ODD: u128 = 15_625;
    let numerator = u128::from(micros.unsigned_abs());
    let mut shift = 0;
    while (numerator << shift) / ODD < 1 << 53 {
        shift += 1;
    }
    let scaled = numerator << shift;
    let (quotient, remainder) = (scaled / ODD, scaled % ODD);
    let mut mantissa = quotient >> 1;
    let round_up = quotient & 1 == 1 && (remainder != 0 || mantissa & 1 == 1);
    if round_up {
        mantissa += 1;
    }
    // An exact power of two from its bits: `powi`'s precision is unspecified.
    let scale = f64::from_bits(((1023 + 1 - shift - 6) as u64) << 52);
    let magnitude = mantissa as f64 * scale;
    if micros < 0 { -magnitude } else { magnitude }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn halfway_microseconds_round_to_even() {
        assert_eq!(from_seconds(1.5e-6), Some(2));
        assert_eq!(from_seconds(2.5e-6), Some(2));
        assert_eq!(from_seconds(5e-7), Some(0));
        assert_eq!(from_seconds(-5e-7), Some(0));
    }

    #[test]
    fn range_is_datetime_range() {
        assert_eq!(from_seconds((MIN / US_PER_SECOND) as f64), Some(MIN as Micros));
        assert_eq!(from_seconds((MIN / US_PER_SECOND) as f64 - 1.0), None);
        assert_eq!(from_seconds(f64::NAN), None);
        assert_eq!(from_seconds(f64::INFINITY), None);
    }

    #[test]
    fn total_seconds_is_correctly_rounded() {
        assert_eq!(to_seconds(2), 2e-6);
        assert_eq!(to_seconds(-1), -1e-6);
        assert_eq!(to_seconds(1_000_000), 1.0);
        assert_eq!(to_seconds(-86_400_000_000), -86_400.0);
        assert_eq!(to_seconds(1_500_000), 1.5);
        assert_eq!(to_seconds(1), 1e-6);
    }
}
