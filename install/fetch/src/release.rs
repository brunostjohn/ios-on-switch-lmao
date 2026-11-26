// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use reqwest::header::RANGE;
use reqwest::{Client, Url};

use crate::error::{Error, Result, http, io_at};
use crate::range;
use crate::span::{Span, fetch_span};
use crate::sums::parse_sums;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Asset {
    pub name: String,
    pub sha256: [u8; 32],
}

/// The length of `url`, from the Content-Range of a one-byte range.
pub async fn content_length(client: &Client, url: &Url) -> Result<u64> {
    let response = client
        .get(url.clone())
        .header(RANGE, range::header(0, 0))
        .send()
        .await
        .map_err(http(url))?;
    range::check_total(url, response.status(), response.headers())
}

/// Each asset at `base` (ending in `/`) into `dir/<name>`. The catalog's hashes are what the files are checked
/// against; the release's SHA256SUMS must list the same ones, which catches a release that was replaced.
pub async fn fetch_release(
    client: &Client,
    base: &Url,
    assets: &[Asset],
    dir: &Path,
    mut progress: impl FnMut(&str, u64),
) -> Result<()> {
    let sums_url = join(base, "SHA256SUMS")?;
    let response = client
        .get(sums_url.clone())
        .send()
        .await
        .and_then(|r| r.error_for_status());
    let sums = parse_sums(
        &response
            .map_err(http(&sums_url))?
            .text()
            .await
            .map_err(http(&sums_url))?,
    )?;
    for asset in assets {
        let listed = sums.get(&asset.name);
        if listed != Some(&asset.sha256) {
            return Err(Error::Release {
                name: asset.name.clone(),
                listed: listed.map(hex::encode),
                pinned: hex::encode(asset.sha256),
            });
        }
        let url = join(base, &asset.name)?;
        let dest = dir.join(&asset.name);
        if tokio::fs::try_exists(&dest).await.map_err(io_at(&dest))? {
            continue;
        }
        let len = content_length(client, &url).await?;
        let span = Span {
            url,
            offset: 0,
            len,
            sha256: asset.sha256,
        };
        fetch_span(client, &span, &dest, |done| progress(&asset.name, done)).await?;
    }
    Ok(())
}

fn join(base: &Url, name: &str) -> Result<Url> {
    let url = base
        .join(name)
        .ok()
        .filter(|url| url.as_str().starts_with(base.as_str()));
    url.ok_or_else(|| Error::Url {
        base: base.to_string(),
        name: name.to_owned(),
    })
}
