// SPDX-License-Identifier: GPL-3.0-or-later
//! Property lists written byte for byte as CPython's `plistlib.dumps` writes them (Python 3.13, the build's
//! interpreter), so a file the build makes with `plistlib` can be made again from the same value.

mod real;

use std::time::{SystemTime, UNIX_EPOCH};

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use plist::{Date, Dictionary, Value};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("strings can't contain control characters")]
    ControlCharacter,
    #[error("a UID has no XML form")]
    Uid,
    #[error("unsupported plist value")]
    Unsupported,
}

const HEADER: &[u8] = b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";

/// `plistlib.dumps(value)`: XML, keys sorted, tab indents.
pub fn to_xml(value: &Value) -> Result<Vec<u8>, Error> {
    let mut writer = XmlWriter {
        out: HEADER.to_vec(),
        level: 0,
    };
    writer.line(b"<plist version=\"1.0\">");
    writer.value(value)?;
    writer.line(b"</plist>");
    Ok(writer.out)
}

struct XmlWriter {
    out: Vec<u8>,
    level: usize,
}

impl XmlWriter {
    fn line(&mut self, text: &[u8]) {
        if !text.is_empty() {
            self.out.extend(std::iter::repeat_n(b'\t', self.level));
            self.out.extend_from_slice(text);
        }
        self.out.push(b'\n');
    }

    fn simple(&mut self, element: &str, text: Option<&str>) -> Result<(), Error> {
        let line = match text {
            Some(text) => format!("<{element}>{}</{element}>", escape(text)?),
            None => format!("<{element}/>"),
        };
        self.line(line.as_bytes());
        Ok(())
    }

    fn begin(&mut self, element: &str) {
        self.line(format!("<{element}>").as_bytes());
        self.level += 1;
    }

    fn end(&mut self, element: &str) {
        self.level -= 1;
        self.line(format!("</{element}>").as_bytes());
    }

    fn value(&mut self, value: &Value) -> Result<(), Error> {
        match value {
            Value::String(text) => self.simple("string", Some(text)),
            Value::Boolean(true) => self.simple("true", None),
            Value::Boolean(false) => self.simple("false", None),
            Value::Integer(integer) => {
                let text = match integer.as_signed() {
                    Some(signed) => signed.to_string(),
                    None => integer.as_unsigned().ok_or(Error::Unsupported)?.to_string(),
                };
                self.simple("integer", Some(&text))
            }
            Value::Real(real) => self.simple("real", Some(&real::repr(*real))),
            Value::Dictionary(dict) => self.dict(dict),
            Value::Data(data) => {
                self.data(data);
                Ok(())
            }
            Value::Date(date) => self.simple("date", Some(&date_text(*date))),
            Value::Array(items) if items.is_empty() => self.simple("array", None),
            Value::Array(items) => {
                self.begin("array");
                for item in items {
                    self.value(item)?;
                }
                self.end("array");
                Ok(())
            }
            Value::Uid(_) => Err(Error::Uid),
            _ => Err(Error::Unsupported),
        }
    }

    fn dict(&mut self, dict: &Dictionary) -> Result<(), Error> {
        if dict.is_empty() {
            return self.simple("dict", None);
        }
        self.begin("dict");
        let mut items: Vec<(&String, &Value)> = dict.iter().collect();
        items.sort_unstable_by(|a, b| a.0.cmp(b.0));
        for (key, value) in items {
            self.simple("key", Some(key))?;
            self.value(value)?;
        }
        self.end("dict");
        Ok(())
    }

    // plistlib wraps base64 at 76 columns less the element's indent, with tabs counted as 8 columns.
    fn data(&mut self, data: &[u8]) {
        self.begin("data");
        self.level -= 1;
        let width = 76usize.saturating_sub(8 * self.level).max(16);
        for chunk in data.chunks(width / 4 * 3) {
            self.line(STANDARD.encode(chunk).as_bytes());
        }
        self.level += 1;
        self.end("data");
    }
}

fn escape(text: &str) -> Result<String, Error> {
    if text
        .chars()
        .any(|c| matches!(c, '\0'..='\x08' | '\x0b' | '\x0c' | '\x0e'..='\x1f'))
    {
        return Err(Error::ControlCharacter);
    }
    Ok(text
        .replace("\r\n", "\n")
        .replace('\r', "\n")
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;"))
}

// plistlib keeps whole seconds, so a fractional time goes to the second before it.
fn date_text(date: Date) -> String {
    let time = SystemTime::from(date);
    let seconds = match time.duration_since(UNIX_EPOCH) {
        Ok(after) => after.as_secs() as i64,
        Err(before) => {
            let before = before.duration();
            -(before.as_secs() as i64) - i64::from(before.subsec_nanos() > 0)
        }
    };
    let (days, day_seconds) = (seconds.div_euclid(86_400), seconds.rem_euclid(86_400));
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        day_seconds / 3600,
        day_seconds / 60 % 60,
        day_seconds % 60
    )
}

// Howard Hinnant's days-to-civil for the proleptic Gregorian calendar.
fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day)
}
