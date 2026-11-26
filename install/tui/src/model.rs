// SPDX-License-Identifier: GPL-3.0-or-later
use std::time::Duration;

/// 16 MiB partition alignment boundary matching hekate's AU alignment.
pub const ALIGN_BYTES: u64 = 16 * 1024 * 1024;
pub const DEFAULT_IOS_BYTES: u64 = 64 * 1024 * 1024 * 1024;
pub const COARSE_STEP_BYTES: u64 = 1024 * 1024 * 1024;
pub const PAGE_STEP_BYTES: u64 = 8 * 1024 * 1024 * 1024;
pub const FINE_STEP_BYTES: u64 = ALIGN_BYTES;

const KIB: u64 = 1024;
const MIB: u64 = 1024 * KIB;
const GIB: u64 = 1024 * MIB;
const TIB: u64 = 1024 * GIB;

#[derive(Debug, thiserror::Error)]
pub enum TuiError {
    #[error("stdout is not an interactive terminal")]
    NotATty,
    #[error("terminal I/O error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Install,
    Partition,
    PartitionAndInstall,
}

impl Mode {
    pub const ALL: [Mode; 3] = [Mode::Install, Mode::Partition, Mode::PartitionAndInstall];

    pub fn needs_build(self) -> bool {
        matches!(self, Mode::Install | Mode::PartitionAndInstall)
    }

    pub fn needs_size(self) -> bool {
        matches!(self, Mode::Partition | Mode::PartitionAndInstall)
    }

    pub fn label(self) -> &'static str {
        match self {
            Mode::Install => "Install iPadOS",
            Mode::Partition => "Partition microSD",
            Mode::PartitionAndInstall => "Partition & Install",
        }
    }

    pub fn short_label(self) -> &'static str {
        match self {
            Mode::Install => "Install",
            Mode::Partition => "Partition",
            Mode::PartitionAndInstall => "Partition + Install",
        }
    }

    pub fn description(self) -> &'static str {
        match self {
            Mode::Install => "Write iPadOS into an existing iOS, iOS-setup, or Linux partition on the card.",
            Mode::Partition => "Shrink the Horizon FAT32 volume and carve a 16 MiB-aligned iOS partition.",
            Mode::PartitionAndInstall => "Shrink FAT32, carve a new iOS partition, and install iPadOS in one pass.",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BuildOption {
    pub id: String,
    pub label: String,
    pub product: String,
    pub release_tag: String,
    pub size_summary: String,
    pub min_partition_bytes: Option<u64>,
    pub unsupported_reason: Option<String>,
}

impl BuildOption {
    pub fn is_supported(&self) -> bool {
        self.unsupported_reason.is_none()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SlotStatus {
    None,
    ExistingIos {
        size_bytes: u64,
        partition_index: Option<u32>,
    },
    ExistingSetup {
        size_bytes: u64,
        partition_index: Option<u32>,
    },
    FreshCandidate {
        name: String,
        size_bytes: u64,
        partition_index: Option<u32>,
    },
}

impl SlotStatus {
    pub fn badge(&self) -> String {
        match self {
            SlotStatus::None => "No iOS slot".to_owned(),
            SlotStatus::ExistingIos { size_bytes, .. } => format!("iOS ({})", format_bytes(*size_bytes)),
            SlotStatus::ExistingSetup { size_bytes, .. } => {
                format!("iOS-setup ({})", format_bytes(*size_bytes))
            }
            SlotStatus::FreshCandidate { name, size_bytes, .. } => {
                format!("{name} ({})", format_bytes(*size_bytes))
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PendingJournal {
    pub stage: String,
    pub detail: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum RelocationEstimate {
    Exact(u64),
    /// Sorted `(ios_bytes, bytes_to_relocate)` sample points interpolated linearly.
    Samples(Vec<(u64, u64)>),
    #[default]
    Proportional,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SizeConstraints {
    pub min_bytes: u64,
    pub max_bytes: u64,
    pub default_bytes: Option<u64>,
    pub total_card_bytes: u64,
    pub fat_total_bytes: u64,
    pub fat_used_bytes: u64,
    pub other_partitions_bytes: u64,
    pub relocate_estimate: RelocationEstimate,
}

impl SizeConstraints {
    pub fn estimate_relocation(&self, ios_bytes: u64) -> u64 {
        match &self.relocate_estimate {
            RelocationEstimate::Exact(bytes) => *bytes,
            RelocationEstimate::Samples(points) => interpolate_samples(points, ios_bytes),
            RelocationEstimate::Proportional => {
                if self.fat_total_bytes == 0 || self.fat_used_bytes == 0 {
                    return 0;
                }
                let carved = ios_bytes.min(self.fat_total_bytes) as u128;
                let est = (self.fat_used_bytes as u128 * carved) / (self.fat_total_bytes as u128);
                (est as u64).min(self.fat_used_bytes)
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CardOption {
    pub id: String,
    pub name: String,
    pub capacity_bytes: u64,
    pub layout_summary: String,
    pub slot: SlotStatus,
    pub pending_journal: Option<PendingJournal>,
    pub confirm_token: String,
    pub confirm_case_sensitive: bool,
    pub size_constraints: Option<SizeConstraints>,
    pub preserved_items: Vec<String>,
    pub overwritten_items: Vec<String>,
    pub unusable_reason: Option<String>,
}

impl CardOption {
    pub fn effective_confirm_token(&self) -> &str {
        if self.confirm_token.is_empty() {
            &self.id
        } else {
            &self.confirm_token
        }
    }

    pub fn matches_confirm(&self, typed: &str) -> bool {
        let expected = self.effective_confirm_token();
        if self.confirm_case_sensitive {
            typed == expected
        } else {
            typed.eq_ignore_ascii_case(expected)
        }
    }
}

#[derive(Clone, Debug)]
pub struct WizardConfig {
    pub title: String,
    pub initial_mode: Option<Mode>,
    pub builds: Vec<BuildOption>,
    pub cards: Vec<CardOption>,
}

impl Default for WizardConfig {
    fn default() -> Self {
        Self {
            title: "iPadOS Switch Installer".to_owned(),
            initial_mode: None,
            builds: Vec::new(),
            cards: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Selection {
    pub mode: Mode,
    pub build_id: Option<String>,
    pub card_id: String,
    pub ios_size_bytes: Option<u64>,
    pub resume_journal: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WizardOutcome {
    Confirmed(Selection),
    Cancelled,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StepDescriptor {
    pub id: String,
    pub label: String,
}

impl StepDescriptor {
    pub fn new(id: impl Into<String>, label: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepStatus {
    Pending,
    Active,
    Done,
    Failed,
    Skipped,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StepState {
    pub id: String,
    pub label: String,
    pub status: StepStatus,
    pub elapsed: Duration,
    pub detail: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DoneSummary {
    pub mode: Mode,
    pub title: String,
    pub card_label: String,
    pub ejected: bool,
    pub details: Vec<String>,
    pub next_steps: Vec<String>,
    pub log_path: Option<String>,
}

impl DoneSummary {
    pub fn effective_next_steps(&self) -> Vec<String> {
        if !self.next_steps.is_empty() {
            return self.next_steps.clone();
        }
        match self.mode {
            Mode::Partition => vec![
                "Re-run the installer in Install mode to write iPadOS into the new partition.".to_owned(),
                "Horizon OS, saves, and emuMMC remain intact on the microSD card.".to_owned(),
            ],
            Mode::Install | Mode::PartitionAndInstall => vec![
                "Insert the microSD card into your Nintendo Switch.".to_owned(),
                "Boot into hekate.".to_owned(),
                "Select More Configs -> iOS to boot iPadOS.".to_owned(),
            ],
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CardStateAfterError {
    Untouched,
    Resumable { detail: String },
    Partial { detail: String },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ErrorSummary {
    pub title: String,
    pub message: String,
    pub log_path: Option<String>,
    pub card_state: CardStateAfterError,
    pub failed_step: Option<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ProgressEvent {
    SetSteps(Vec<StepDescriptor>),
    StepStarted {
        step_id: String,
        irreversible: bool,
    },
    StepProgress {
        step_id: String,
        done_bytes: u64,
        total_bytes: Option<u64>,
        bytes_per_sec: Option<f64>,
        detail: Option<String>,
    },
    StepFinished {
        step_id: String,
        status: StepStatus,
        elapsed: Option<Duration>,
    },
    SetIrreversible(bool),
    Log(String),
    Finished(DoneSummary),
    Failed(ErrorSummary),
    Cancelled {
        message: String,
        log_path: Option<String>,
    },
}

pub fn align_down(value: u64, alignment: u64) -> u64 {
    if alignment == 0 {
        return value;
    }
    value - (value % alignment)
}

pub fn align_up(value: u64, alignment: u64) -> u64 {
    if alignment == 0 {
        return value;
    }
    let rem = value % alignment;
    if rem == 0 {
        value
    } else {
        value.saturating_add(alignment - rem)
    }
}

pub fn snap_nearest(value: u64, alignment: u64) -> u64 {
    if alignment == 0 {
        return value;
    }
    let rem = value % alignment;
    let down = value - rem;
    if rem.saturating_mul(2) >= alignment {
        down.saturating_add(alignment)
    } else {
        down
    }
}

pub fn format_bytes(bytes: u64) -> String {
    if bytes >= TIB {
        format!("{:.2} TiB", bytes as f64 / TIB as f64)
    } else if bytes >= GIB {
        format!("{:.2} GiB", bytes as f64 / GIB as f64)
    } else if bytes >= MIB {
        format!("{:.1} MiB", bytes as f64 / MIB as f64)
    } else if bytes >= KIB {
        format!("{:.1} KiB", bytes as f64 / KIB as f64)
    } else {
        format!("{bytes} B")
    }
}

pub fn format_gib(bytes: u64) -> String {
    let mib = bytes / MIB;
    if bytes.is_multiple_of(GIB) {
        format!("{} GiB", bytes / GIB)
    } else if bytes.is_multiple_of(MIB) {
        format!("{:.2} GiB ({mib} MiB)", bytes as f64 / GIB as f64)
    } else {
        format!("{:.2} GiB", bytes as f64 / GIB as f64)
    }
}

pub fn format_duration(duration: Duration) -> String {
    let total_secs = duration.as_secs();
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;
    if hours > 0 {
        format!("{hours}:{mins:02}:{secs:02}")
    } else {
        format!("{mins:02}:{secs:02}")
    }
}

fn interpolate_samples(points: &[(u64, u64)], target: u64) -> u64 {
    if points.is_empty() {
        return 0;
    }
    if target <= points[0].0 {
        return points[0].1;
    }
    let last = points[points.len() - 1];
    if target >= last.0 {
        return last.1;
    }
    for pair in points.windows(2) {
        let (x0, y0) = pair[0];
        let (x1, y1) = pair[1];
        if target >= x0 && target <= x1 {
            if x1 == x0 {
                return y1;
            }
            let num = (target - x0) as u128;
            let den = (x1 - x0) as u128;
            return if y1 >= y0 {
                y0 + (((y1 - y0) as u128 * num) / den) as u64
            } else {
                y0 - (((y0 - y1) as u128 * num) / den) as u64
            };
        }
    }
    last.1
}
