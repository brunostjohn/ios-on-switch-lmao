// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;
use std::path::PathBuf;

#[derive(Debug, thiserror::Error)]
pub enum SeedError {
    #[error("operation cancelled")]
    Cancelled,
    #[error("invalid seed asset: {0}")]
    Asset(String),
    #[error("seed asset build {got:?} does not match target build {expected:?}")]
    BuildMismatch { got: String, expected: String },
    #[error("seed asset QuartzCore digest {got} does not match pinned {expected}")]
    QuartzCoreMismatch { got: String, expected: String },
    #[error("GPU bundle stack key {bundle} does not match seed asset stack key {asset}")]
    StackKeyMismatch { bundle: String, asset: String },
    #[error("seed FFI error ({op}): {detail}")]
    Ffi { op: &'static str, detail: String },
    #[error("crawl: {0}")]
    Crawl(#[from] switch_seed_crawl::CrawlError),
    #[error("plan: {0}")]
    Plan(#[from] switch_seed_plan::PlanError),
    #[error("link: {0}")]
    Link(#[from] switch_seed_link::LinkError),
    #[error("JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("plist: {0}")]
    Plist(#[from] plist::Error),
    #[error("thread pool: {0}")]
    ThreadPool(String),
    #[error("{path}: {source}")]
    IoPath {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(transparent)]
    Io(#[from] io::Error),
}

pub type Result<T> = std::result::Result<T, SeedError>;

impl SeedError {
    pub fn io_path(path: impl Into<PathBuf>, source: io::Error) -> Self {
        Self::IoPath {
            path: path.into(),
            source,
        }
    }
}
