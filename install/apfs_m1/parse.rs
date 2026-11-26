// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_write::CheckpointAreas;

use crate::build::GenFile;

pub fn uuid_string(u: &[u8; 16]) -> String {
    let h: String = u.iter().map(|b| format!("{b:02X}")).collect();
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}

pub fn parse_uuid(s: &str) -> Result<[u8; 16], String> {
    let hex: String = s.chars().filter(|c| *c != '-').collect();
    if hex.len() != 32 {
        return Err(format!("not a UUID: {s}"));
    }
    let mut out = [0u8; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|_| format!("not a UUID: {s}"))?;
    }
    Ok(out)
}

pub fn parse_int<T: TryFrom<u64>>(s: &str) -> Result<T, String> {
    let v = match s.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16),
        None => s.parse(),
    }
    .map_err(|_| format!("not a number: {s}"))?;
    T::try_from(v).map_err(|_| format!("out of range: {s}"))
}

pub fn parse_areas(s: &str) -> Result<CheckpointAreas, String> {
    match s {
        "rule" => Ok(CheckpointAreas::Rule),
        "keep" => Ok(CheckpointAreas::Keep),
        "mac-composed" => Ok(CheckpointAreas::MAC_COMPOSED),
        _ => {
            let (desc, data) = s
                .split_once(':')
                .ok_or_else(|| format!("not rule, keep, mac-composed or DESC:DATA: {s}"))?;
            Ok(CheckpointAreas::Sized {
                desc: parse_int(desc)?,
                data: parse_int(data)?,
            })
        }
    }
}

pub fn parse_file(s: &str) -> Result<GenFile, String> {
    let mut parts = s.rsplitn(3, ':');
    let (Some(seed), Some(bytes), Some(name)) = (parts.next(), parts.next(), parts.next()) else {
        return Err(format!("not NAME:BYTES:SEED: {s}"));
    };
    Ok(GenFile {
        name: name.into(),
        bytes: parse_int(bytes)?,
        seed: parse_int(seed)?,
    })
}
