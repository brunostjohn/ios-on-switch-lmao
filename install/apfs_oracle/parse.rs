// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;

use switch_apfs_write::CheckpointAreas;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SealedCryptexSpec {
    pub name: String,
    pub source: PathBuf,
    pub offset: u64,
    pub uuid: [u8; 16],
    pub manifest: Option<PathBuf>,
    pub vol_name: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VerbatimCryptexSpec {
    pub name: String,
    pub path: PathBuf,
    pub sha256: Option<[u8; 32]>,
}

pub fn uuid_string(u: &[u8; 16]) -> String {
    let h = hex::encode_upper(u);
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}

pub fn parse_uuid(s: &str) -> Result<[u8; 16], String> {
    let stripped: String = s.chars().filter(|&c| c != '-').collect();
    if stripped.len() != 32 {
        return Err(format!("not a UUID: {s}"));
    }
    let mut out = [0u8; 16];
    hex::decode_to_slice(&stripped, &mut out).map_err(|_| format!("not a UUID: {s}"))?;
    Ok(out)
}

pub fn parse_sha256(s: &str) -> Result<[u8; 32], String> {
    if s.len() != 64 {
        return Err(format!("sha256 must be 64 hex chars: {s}"));
    }
    let mut out = [0u8; 32];
    hex::decode_to_slice(s, &mut out).map_err(|_| format!("invalid sha256 hex: {s}"))?;
    Ok(out)
}

pub fn parse_int<T: TryFrom<u64>>(s: &str) -> Result<T, String> {
    let v = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
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
                .ok_or_else(|| format!("expected rule, keep, mac-composed or DESC:DATA, got {s}"))?;
            Ok(CheckpointAreas::Sized {
                desc: parse_int(desc)?,
                data: parse_int(data)?,
            })
        }
    }
}

/// Parses comma-separated `key=value` fields for a sealed 27 cryptex:
/// `name=NAME,source=PATH,uuid=UUID[,manifest=PATH][,vol_name=NAME][,offset=BYTES]`.
pub fn parse_sealed_cryptex(spec: &str) -> Result<SealedCryptexSpec, String> {
    let mut name = None;
    let mut source = None;
    let mut offset = 0u64;
    let mut uuid = None;
    let mut manifest = None;
    let mut vol_name = None;

    for part in spec.split(',') {
        let (k, v) = part
            .split_once('=')
            .ok_or_else(|| format!("expected key=value in sealed cryptex spec {spec:?}"))?;
        if v.is_empty() {
            return Err(format!("empty value for {k} in {spec:?}"));
        }
        match k {
            "name" => name = Some(v.to_owned()),
            "source" => source = Some(PathBuf::from(v)),
            "offset" => offset = parse_int(v)?,
            "uuid" => uuid = Some(parse_uuid(v)?),
            "manifest" => manifest = Some(PathBuf::from(v)),
            "vol_name" => vol_name = Some(v.to_owned()),
            _ => return Err(format!("unknown key {k:?} in sealed cryptex spec")),
        }
    }

    Ok(SealedCryptexSpec {
        name: name.ok_or("sealed cryptex spec missing name=")?,
        source: source.ok_or("sealed cryptex spec missing source=")?,
        offset,
        uuid: uuid.ok_or("sealed cryptex spec missing uuid=")?,
        manifest,
        vol_name,
    })
}

/// Parses comma-separated `key=value` fields for a verbatim 17 cryptex:
/// `name=NAME,path=PATH[,sha256=HEX]`.
pub fn parse_verbatim_cryptex(spec: &str) -> Result<VerbatimCryptexSpec, String> {
    let mut name = None;
    let mut path = None;
    let mut sha256 = None;

    for part in spec.split(',') {
        let (k, v) = part
            .split_once('=')
            .ok_or_else(|| format!("expected key=value in verbatim cryptex spec {spec:?}"))?;
        if v.is_empty() {
            return Err(format!("empty value for {k} in {spec:?}"));
        }
        match k {
            "name" => name = Some(v.to_owned()),
            "path" => path = Some(PathBuf::from(v)),
            "sha256" => sha256 = Some(parse_sha256(v)?),
            _ => return Err(format!("unknown key {k:?} in verbatim cryptex spec")),
        }
    }

    Ok(VerbatimCryptexSpec {
        name: name.ok_or("verbatim cryptex spec missing name=")?,
        path: path.ok_or("verbatim cryptex spec missing path=")?,
        sha256,
    })
}
