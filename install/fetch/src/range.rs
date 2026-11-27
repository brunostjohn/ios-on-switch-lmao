// SPDX-License-Identifier: GPL-3.0-or-later
use reqwest::header::{CONTENT_RANGE, HeaderMap};
use reqwest::{StatusCode, Url};

use crate::error::{Error, Result};

pub(crate) fn header(first: u64, last: u64) -> String {
    format!("bytes={first}-{last}")
}

fn parse_content_range(value: &str) -> Option<(u64, u64, Option<u64>)> {
    let (span, total) = value.strip_prefix("bytes ")?.split_once('/')?;
    let (first, last) = span.split_once('-')?;
    let total = match total {
        "*" => None,
        total => Some(total.parse().ok()?),
    };
    Some((first.parse().ok()?, last.parse().ok()?, total))
}

/// Accepts only a 206 whose Content-Range is exactly `first-last`, and gives its total length when the server names it.
pub(crate) fn check(url: &Url, status: StatusCode, headers: &HeaderMap, first: u64, last: u64) -> Result<Option<u64>> {
    if status != StatusCode::PARTIAL_CONTENT {
        return Err(Error::NotPartial {
            url: url.to_string(),
            status: status.as_u16(),
            first,
            last,
        });
    }
    match content_range(headers).and_then(parse_content_range) {
        Some((a, b, total)) if a == first && b == last => Ok(total),
        _ => Err(Error::ContentRange {
            url: url.to_string(),
            got: content_range(headers).map(str::to_owned),
            first,
            last,
        }),
    }
}

/// The total length a 206 for `bytes=0-0` names.
pub(crate) fn check_total(url: &Url, status: StatusCode, headers: &HeaderMap) -> Result<u64> {
    check(url, status, headers, 0, 0)?.ok_or_else(|| Error::ContentRange {
        url: url.to_string(),
        got: content_range(headers).map(str::to_owned),
        first: 0,
        last: 0,
    })
}

fn content_range(headers: &HeaderMap) -> Option<&str> {
    headers.get(CONTENT_RANGE).and_then(|value| value.to_str().ok())
}
