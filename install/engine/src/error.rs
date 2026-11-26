// SPDX-License-Identifier: GPL-3.0-or-later
use std::io;
use std::path::Path;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CardErrorState {
    Untouched,
    Resumable(String),
    Partial(String),
}

#[derive(Debug, thiserror::Error)]
pub enum EngineError {
    #[error("operation cancelled")]
    Cancelled,
    #[error("card has an interrupted partition journal ({stage}): {detail}; resume it first")]
    PendingJournal { stage: String, detail: String },
    #[error(transparent)]
    UnsupportedBuild(#[from] switch_catalog::Unsupported),
    #[error("catalog: {0}")]
    Catalog(#[from] switch_catalog::Error),
    #[error("fetch: {0}")]
    Fetch(#[from] switch_fetch::Error),
    #[error("IPSW: {0}")]
    Ipsw(#[from] switch_ipsw::Error),
    #[error("AEA: {0}")]
    Aea(#[from] switch_aea::Error),
    #[error("IM4P: {0}")]
    Img4(#[from] switch_img4::Error),
    #[error("boot files: {0}")]
    Boot(#[from] switch_boot::Error),
    #[error("pretranslation: {0}")]
    Translate(#[from] switch_translate::Error),
    #[error("decmpfs: {0}")]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error("APFS read: {0}")]
    ApfsRead(#[from] switch_apfs_read::ReadError),
    #[error("APFS write: {0}")]
    ApfsWrite(#[from] switch_apfs_write::WriteError),
    #[error("APFS seal: {0}")]
    ApfsSeal(#[from] switch_apfs_seal::SealError),
    #[error("image: {0}")]
    Image(#[from] switch_image::Error),
    #[error("pretranslated overlay: {0}")]
    Pretranslated(#[from] switch_image_pretranslated::Error),
    #[error("overlay: {0}")]
    Overlay(#[from] switch_image_overlay::Error),
    #[error("bootstrap: {0}")]
    Bootstrap(#[from] switch_image_bootstrap::Error),
    #[error("GPU stage: {0}")]
    ImageGpu(#[from] switch_image_gpu::Error),
    #[error("disk: {0}")]
    Disk(#[from] switch_disk::Error),
    #[error("card tables: {0}")]
    Card(#[from] switch_card::Error),
    #[error("boot volume: {0}")]
    Sd(#[from] switch_sd::Error),
    #[error("partition: {0}")]
    Partition(#[from] switch_partition::Error),
    #[error("HTTP: {0}")]
    Http(String),
    #[error("configuration: {0}")]
    Config(String),
    #[error("resume state: {0}")]
    ResumeState(String),
    #[error("{context}: {source}")]
    Io {
        context: String,
        #[source]
        source: io::Error,
    },
}

pub type Result<T> = std::result::Result<T, EngineError>;

impl EngineError {
    pub fn io(context: impl Into<String>, source: io::Error) -> Self {
        Self::Io {
            context: context.into(),
            source,
        }
    }

    pub fn io_path(path: &Path, source: io::Error) -> Self {
        Self::Io {
            context: path.display().to_string(),
            source,
        }
    }

    pub fn card_state(&self, irreversible: bool) -> CardErrorState {
        if !irreversible {
            return match self {
                Self::PendingJournal { detail, .. } => CardErrorState::Resumable(detail.clone()),
                _ => CardErrorState::Untouched,
            };
        }
        match self {
            Self::Partition(_) | Self::PendingJournal { .. } => CardErrorState::Resumable(
                "Partition journal is intact on the card; re-run to resume safely.".to_owned(),
            ),
            Self::ApfsWrite(_) | Self::Sd(_) | Self::Card(_) => CardErrorState::Resumable(
                "Target partition is marked iOS-setup; re-run install to finish cleanly.".to_owned(),
            ),
            _ => CardErrorState::Partial(
                "An error occurred after card writes began; re-run the installer to recover.".to_owned(),
            ),
        }
    }
}
