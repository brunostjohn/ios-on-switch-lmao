// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sha2::Sha256;
use sha2::digest::common::hazmat::{SerializableState, SerializedState};
use tokio::io::AsyncWriteExt;

use crate::span::Span;

pub(crate) struct Paths {
    pub dest: PathBuf,
    pub part: PathBuf,
    pub state: PathBuf,
    state_tmp: PathBuf,
}

impl Paths {
    pub fn new(dest: &Path) -> Self {
        let with = |suffix: &str| {
            let mut path = dest.as_os_str().to_owned();
            path.push(suffix);
            PathBuf::from(path)
        };
        Paths {
            dest: dest.to_owned(),
            part: with(".part"),
            state: with(".state"),
            state_tmp: with(".state.tmp"),
        }
    }
}

/// The `.state` file. It names the span it belongs to, so a checkpoint is never applied to a different download.
#[derive(Serialize, Deserialize)]
struct Record {
    url: String,
    offset: u64,
    len: u64,
    sha256: String,
    done: u64,
    midstate: String,
}

pub(crate) struct Checkpoint {
    pub done: u64,
    pub hasher: Sha256,
}

pub(crate) async fn load(paths: &Paths, span: &Span) -> Option<Checkpoint> {
    let record: Record = serde_json::from_slice(&tokio::fs::read(&paths.state).await.ok()?).ok()?;
    let same = record.url == span.url.as_str()
        && record.offset == span.offset
        && record.len == span.len
        && record.sha256 == hex::encode(span.sha256)
        && record.done <= span.len;
    if !same {
        return None;
    }
    let midstate = hex::decode(&record.midstate).ok()?;
    let hasher = Sha256::deserialize(&SerializedState::<Sha256>::try_from(&midstate[..]).ok()?).ok()?;
    Some(Checkpoint {
        done: record.done,
        hasher,
    })
}

/// Written only after the `.part` bytes it counts are on disk, and replaced atomically.
pub(crate) async fn save(paths: &Paths, span: &Span, done: u64, hasher: &Sha256) -> io::Result<()> {
    let record = Record {
        url: span.url.to_string(),
        offset: span.offset,
        len: span.len,
        sha256: hex::encode(span.sha256),
        done,
        midstate: hex::encode(hasher.serialize()),
    };
    let mut file = tokio::fs::File::create(&paths.state_tmp).await?;
    file.write_all(&serde_json::to_vec(&record)?).await?;
    file.sync_all().await?;
    drop(file);
    tokio::fs::rename(&paths.state_tmp, &paths.state).await
}

pub(crate) async fn remove(path: &Path) -> io::Result<()> {
    match tokio::fs::remove_file(path).await {
        Err(error) if error.kind() != io::ErrorKind::NotFound => Err(error),
        _ => Ok(()),
    }
}
