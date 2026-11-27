// SPDX-License-Identifier: GPL-3.0-or-later
//! The three LaunchDaemons the catalogue gains, checked as `tools/package/launchd_catalog.py` checks them: with
//! Python's `==` (so `1` and `1.0` equal `True`) where it compares and `is` where it asks for exactly `True` or `False`.

use std::io::Cursor;

use plist::Value;

use crate::Error;

pub const LABEL: &str = "com.brunostjohn.switch.bootstrap";
pub const JOB_PATH: &str = "/System/Library/LaunchDaemons/com.brunostjohn.switch.bootstrap.plist";
pub const APPS_LABEL: &str = "com.brunostjohn.switch.bootstrap-apps";
pub const APPS_JOB_PATH: &str = "/System/Library/LaunchDaemons/com.brunostjohn.switch.bootstrap-apps.plist";
pub const SDWAKE_LABEL: &str = "com.brunostjohn.switch.sdwake";
pub const SDWAKE_JOB_PATH: &str = "/System/Library/LaunchDaemons/com.brunostjohn.switch.sdwake.plist";

enum Literal {
    Str(&'static str),
    True,
    List(&'static [Literal]),
    Dict(&'static [(&'static str, Literal)]),
}

const SDWAKE_MATCH: Literal = Literal::Dict(&[(
    "com.apple.iokit.matching",
    Literal::Dict(&[(
        "com.brunostjohn.switch.sdwake.shared-volume",
        Literal::Dict(&[
            ("IOMatchLaunchStream", Literal::True),
            ("IOProviderClass", Literal::Str("IOMedia")),
            (
                "IOParentMatch",
                Literal::Dict(&[
                    ("IOProviderClass", Literal::Str("IOBlockStorageDevice")),
                    ("IONameMatch", Literal::Str("SwitchSDSharedVolume")),
                ]),
            ),
        ]),
    )]),
)]);

fn equals(value: Option<&Value>, literal: &Literal) -> bool {
    match (value, literal) {
        (Some(Value::String(value)), Literal::Str(literal)) => value == literal,
        (Some(Value::Boolean(value)), Literal::True) => *value,
        (Some(Value::Integer(value)), Literal::True) => value.as_signed() == Some(1),
        (Some(Value::Real(value)), Literal::True) => *value == 1.0,
        (Some(Value::Array(items)), Literal::List(literal)) => {
            items.len() == literal.len() && items.iter().zip(*literal).all(|(item, l)| equals(Some(item), l))
        }
        (Some(Value::Dictionary(dict)), Literal::Dict(literal)) => {
            dict.len() == literal.len() && literal.iter().all(|(key, l)| equals(dict.get(key), l))
        }
        _ => false,
    }
}

fn is_true(value: Option<&Value>) -> bool {
    matches!(value, Some(Value::Boolean(true)))
}

/// `job.get(key)`, or `None` for a job that is no dictionary (where Python's `.get` raises).
fn field<'a>(job: &'a Value, key: &str) -> Option<&'a Value> {
    job.as_dictionary().and_then(|dict| dict.get(key))
}

/// `job.get(key, False) is False`.
fn absent_or_false(value: Option<&Value>) -> bool {
    matches!(value, None | Some(Value::Boolean(false)))
}

/// The three job plists, read as plistlib reads them.
pub struct Jobs {
    pub bootstrap: Value,
    pub apps: Value,
    pub sdwake: Value,
}

impl Jobs {
    pub fn parse(bootstrap: &[u8], apps: &[u8], sdwake: &[u8]) -> Result<Jobs, Error> {
        let parse = |name: &'static str, data: &[u8]| {
            Value::from_reader(Cursor::new(data)).map_err(|source| Error::JobPlist { name, source })
        };
        Ok(Jobs {
            bootstrap: parse("bootstrap", bootstrap)?,
            apps: parse("bootstrap-apps", apps)?,
            sdwake: parse("sdwake", sdwake)?,
        })
    }

    /// Each job with the catalogue path it goes in, once all three pass `add_bootstrap_jobs`' checks.
    pub fn checked(&self) -> Result<[(&'static str, &Value); 3], Error> {
        let job = &self.bootstrap;
        if !equals(field(job, "Label"), &Literal::Str(LABEL))
            || !equals(
                field(job, "ProgramArguments"),
                &Literal::List(&[Literal::Str("/usr/libexec/switch-bootstrap")]),
            )
            || !equals(field(job, "UserName"), &Literal::Str("root"))
            || !is_true(field(job, "RunAtLoad"))
        {
            return Err(Error::InvalidJob("bootstrap LaunchDaemon"));
        }
        let job = &self.apps;
        if !equals(field(job, "Label"), &Literal::Str(APPS_LABEL))
            || !equals(
                field(job, "ProgramArguments"),
                &Literal::List(&[
                    Literal::Str("/usr/libexec/switch-appregd"),
                    Literal::Str("--bootstrap-service"),
                ]),
            )
            || !equals(field(job, "UserName"), &Literal::Str("mobile"))
            || !equals(
                field(job, "MachServices"),
                &Literal::Dict(&[(APPS_LABEL, Literal::True)]),
            )
            || !is_true(field(job, "MachServices").and_then(|services| field(services, APPS_LABEL)))
            || !absent_or_false(field(job, "RunAtLoad"))
            || !absent_or_false(field(job, "KeepAlive"))
        {
            return Err(Error::InvalidJob("bootstrap app registration LaunchDaemon"));
        }
        let job = &self.sdwake;
        if !equals(field(job, "Label"), &Literal::Str(SDWAKE_LABEL))
            || !equals(
                field(job, "ProgramArguments"),
                &Literal::List(&[Literal::Str("/usr/libexec/switch-sdwake")]),
            )
            || !equals(field(job, "UserName"), &Literal::Str("root"))
            || !equals(field(job, "LaunchEvents"), &SDWAKE_MATCH)
            || !absent_or_false(field(job, "RunAtLoad"))
            || !absent_or_false(field(job, "KeepAlive"))
        {
            return Err(Error::InvalidJob("shared SD wake LaunchDaemon"));
        }
        Ok([
            (JOB_PATH, &self.bootstrap),
            (APPS_JOB_PATH, &self.apps),
            (SDWAKE_JOB_PATH, &self.sdwake),
        ])
    }
}
