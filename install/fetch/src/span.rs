// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::SeekFrom;
use std::path::Path;

use reqwest::header::RANGE;
use reqwest::{Client, Url};
use sha2::{Digest, Sha256};
use tokio::fs::{File, OpenOptions};
use tokio::io::{AsyncSeekExt, AsyncWriteExt};

use crate::error::{Error, Result, http, io_at};
use crate::range;
use crate::state::{self, Checkpoint, Paths};

pub const CHECKPOINT: u64 = 64 << 20;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Span {
    pub url: Url,
    pub offset: u64,
    pub len: u64,
    pub sha256: [u8; 32],
}

impl Span {
    /// A member's stored bytes in the IPSW at `url`.
    pub fn member(url: &Url, member: &switch_catalog::Member) -> Span {
        Span {
            url: url.clone(),
            offset: member.offset,
            len: member.stored_size,
            sha256: member.sha256.0,
        }
    }
}

/// Downloads `span` into `dest`, resuming from `dest.part` and `dest.state` when an earlier call left them. A present
/// `dest` counts as done: only a download whose SHA-256 matched is ever renamed to it. `progress` gets the bytes held.
pub async fn fetch_span(client: &Client, span: &Span, dest: &Path, progress: impl FnMut(u64)) -> Result<()> {
    fetch_span_with(client, span, dest, CHECKPOINT, progress).await
}

pub async fn fetch_span_with(
    client: &Client,
    span: &Span,
    dest: &Path,
    checkpoint: u64,
    mut progress: impl FnMut(u64),
) -> Result<()> {
    assert!(checkpoint > 0);
    if tokio::fs::try_exists(dest).await.map_err(io_at(dest))? {
        progress(span.len);
        return Ok(());
    }
    let paths = Paths::new(dest);
    let (mut file, mut at) = open(&paths, span).await?;
    progress(at.done);
    if at.done < span.len {
        let fetched = download(client, span, &paths, &mut file, &mut at, checkpoint, &mut progress).await;
        if let Err(error) = fetched {
            // What arrived is good: keep it for the next call. A local I/O error is left out because tokio reports a
            // failed write on a later call, after `done` has counted it.
            if !matches!(error, Error::Io { .. }) && flush(&mut file).await.is_ok() {
                let _ = state::save(&paths, span, at.done, &at.hasher).await;
            }
            return Err(error);
        }
    }
    finish(&paths, span, file, at.hasher).await
}

/// The bytes of `span` that `dest.state` holds, if it belongs to this span.
pub async fn checkpointed(dest: &Path, span: &Span) -> Option<u64> {
    state::load(&Paths::new(dest), span).await.map(|at| at.done)
}

async fn open(paths: &Paths, span: &Span) -> Result<(File, Checkpoint)> {
    if let Some(at) = state::load(paths, span).await
        && let Ok(mut file) = OpenOptions::new().write(true).open(&paths.part).await
        && file.metadata().await.is_ok_and(|meta| meta.len() >= at.done)
    {
        // A crash after the last checkpoint leaves more bytes than it counts; they are fetched again.
        file.set_len(at.done).await.map_err(io_at(&paths.part))?;
        file.seek(SeekFrom::Start(at.done)).await.map_err(io_at(&paths.part))?;
        return Ok((file, at));
    }
    state::remove(&paths.state).await.map_err(io_at(&paths.state))?;
    let file = File::create(&paths.part).await.map_err(io_at(&paths.part))?;
    Ok((
        file,
        Checkpoint {
            done: 0,
            hasher: Sha256::new(),
        },
    ))
}

async fn download(
    client: &Client,
    span: &Span,
    paths: &Paths,
    file: &mut File,
    at: &mut Checkpoint,
    checkpoint: u64,
    progress: &mut impl FnMut(u64),
) -> Result<()> {
    let (first, last) = (span.offset + at.done, span.offset + span.len - 1);
    let mut response = client
        .get(span.url.clone())
        .header(RANGE, range::header(first, last))
        .send()
        .await
        .map_err(http(&span.url))?;
    range::check(&span.url, response.status(), response.headers(), first, last)?;
    let mut next = (at.done / checkpoint + 1) * checkpoint;
    while let Some(chunk) = response.chunk().await.map_err(http(&span.url))? {
        if chunk.len() as u64 > span.len - at.done {
            let got = at.done + chunk.len() as u64;
            return Err(Error::BodyLength {
                url: span.url.to_string(),
                got,
                expected: span.len,
            });
        }
        let mut rest = &chunk[..];
        while !rest.is_empty() {
            let take = rest.len().min((next - at.done) as usize);
            file.write_all(&rest[..take]).await.map_err(io_at(&paths.part))?;
            at.hasher.update(&rest[..take]);
            at.done += take as u64;
            rest = &rest[take..];
            if at.done == next {
                flush(file).await.map_err(io_at(&paths.part))?;
                state::save(paths, span, at.done, &at.hasher)
                    .await
                    .map_err(io_at(&paths.state))?;
                next += checkpoint;
            }
        }
        progress(at.done);
    }
    if at.done != span.len {
        return Err(Error::BodyLength {
            url: span.url.to_string(),
            got: at.done,
            expected: span.len,
        });
    }
    flush(file).await.map_err(io_at(&paths.part))?;
    state::save(paths, span, at.done, &at.hasher)
        .await
        .map_err(io_at(&paths.state))
}

async fn flush(file: &mut File) -> std::io::Result<()> {
    file.flush().await?;
    file.sync_data().await
}

async fn finish(paths: &Paths, span: &Span, mut file: File, hasher: Sha256) -> Result<()> {
    flush(&mut file).await.map_err(io_at(&paths.part))?;
    drop(file);
    let got: [u8; 32] = hasher.finalize().into();
    if got != span.sha256 {
        state::remove(&paths.part).await.map_err(io_at(&paths.part))?;
        state::remove(&paths.state).await.map_err(io_at(&paths.state))?;
        return Err(Error::HashMismatch {
            path: paths.dest.clone(),
            got: hex::encode(got),
            expected: hex::encode(span.sha256),
        });
    }
    tokio::fs::rename(&paths.part, &paths.dest)
        .await
        .map_err(io_at(&paths.dest))?;
    state::remove(&paths.state).await.map_err(io_at(&paths.state))
}
