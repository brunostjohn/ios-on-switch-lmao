// SPDX-License-Identifier: GPL-3.0-or-later
pub mod assemble;
pub mod boot;
pub mod decode;
pub mod error;
pub mod fetch;
pub mod modes;
pub mod paths;
pub mod pretranslate;
pub mod probe;
pub mod progress;
pub mod resume;
pub mod target;

use std::path::{Path, PathBuf};

use switch_catalog::{Build, Catalog, Role};
use switch_partition::minimum_ios_bytes;

pub use error::{CardErrorState, EngineError, Result};
pub use fetch::{
    HTTP_CONNECT_TIMEOUT, HTTP_READ_TIMEOUT, PreparedAssets, build_http_client, load_catalog, resolve_release_asset,
};
pub use modes::steps_for_mode;
pub use paths::{APP_DATA_DIR_NAME, RunLog, default_cache_dir, default_log_dir, timestamped_log_path};
pub use probe::{CardProbe, ProbeJournal, ProbeSize, ProbeSlot, probe_device, probe_image, read_pending_journal};
pub use progress::{CancelToken, EngineEvent, EngineProgress, EngineStep, NoopSink, ProgressSink, StepOutcome};
pub use resume::{BlockRange, CachedFileStamp, RESUME_VERSION, ResumeState, ResumeStore, partition_fingerprint};
pub use target::{OpenedTarget, PartitionWindow, TargetSpec};

/// `diagnostics/switch/SwitchRawMount.h` `SWITCH_RAW_DATA_UUID`.
pub const DATA_VOLUME_UUID_STR: &str = "D1B1D053-D0A6-496C-8625-A947A3DA863E";
/// `drivers/NativeSwitchBlock/SwitchCryptex.h` `SWITCH_PREBOOT_UUID`.
pub const PREBOOT_VOLUME_UUID_STR: &str = "4AA4A761-5138-439D-9A30-D4138F6887FA";
pub const DEFAULT_TIME_NS: u64 = 1_795_000_000_000_000_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Install,
    Partition,
    PartitionAndInstall,
}

impl Mode {
    pub fn needs_build(self) -> bool {
        matches!(self, Mode::Install | Mode::PartitionAndInstall)
    }

    pub fn needs_partition(self) -> bool {
        matches!(self, Mode::Partition | Mode::PartitionAndInstall)
    }
}

#[derive(Clone, Debug, Default)]
pub struct LocalInputs {
    pub catalog_path: Option<PathBuf>,
    pub catalog_url: Option<String>,
    pub release_tag: Option<String>,
    pub assets_dir: Option<PathBuf>,
    pub boot_tar: Option<PathBuf>,
    pub boot_files_dir: Option<PathBuf>,
    pub ipsw_path: Option<PathBuf>,
    pub os_dmg: Option<PathBuf>,
    pub system_cryptex: Option<PathBuf>,
    pub app_cryptex: Option<PathBuf>,
    pub overlay_dir: Option<PathBuf>,
    pub overlay_builtin_pin: Option<PathBuf>,
    pub overlay_seed_pin: Option<PathBuf>,
    pub bootstrap_dir: Option<PathBuf>,
    pub gpu_dir: Option<PathBuf>,
    pub shader_seed_dir: Option<PathBuf>,
    pub pretranslate_edits: Option<PathBuf>,
    pub translator_version: Option<String>,
    pub pretranslated_os_dir: Option<PathBuf>,
    pub pretranslated_system_cryptex_dir: Option<PathBuf>,
    pub pretranslated_app_cryptex_dir: Option<PathBuf>,
}

#[derive(Clone, Debug)]
pub struct RunRequest {
    pub mode: Mode,
    pub target: TargetSpec,
    pub build_id: Option<String>,
    pub partition_index: Option<u32>,
    pub ios_size_bytes: Option<u64>,
    pub resume_journal: bool,
    pub work_dir: Option<PathBuf>,
    pub log_path: Option<PathBuf>,
    pub time_ns: u64,
    pub local: LocalInputs,
    pub image_used_override: Option<u64>,
}

impl RunRequest {
    pub fn new(mode: Mode, target: TargetSpec) -> Self {
        Self {
            mode,
            target,
            build_id: None,
            partition_index: None,
            ios_size_bytes: None,
            resume_journal: false,
            work_dir: None,
            log_path: None,
            time_ns: DEFAULT_TIME_NS,
            local: LocalInputs::default(),
            image_used_override: None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RunSummary {
    pub mode: Mode,
    pub card_label: String,
    pub build: Option<String>,
    pub ejected: bool,
    pub resumed_journal: bool,
    pub partition_index: Option<u32>,
    pub partition_bytes: Option<u64>,
    pub boot_files_written: usize,
    pub log_path: Option<PathBuf>,
}

pub fn estimate_image_used_bytes(build: &Build) -> u64 {
    let os = build.container(Role::Os).map_or(0, |(_, p)| p.size);
    let sys = build.container(Role::SystemCryptex).map_or(0, |(_, p)| p.size);
    let app = build.container(Role::AppCryptex).map_or(0, |(_, p)| p.size);
    os.saturating_add(sys).saturating_add(app)
}

pub fn estimate_min_partition_bytes(build: &Build) -> u64 {
    minimum_ios_bytes(estimate_image_used_bytes(build))
}

pub fn select_build(catalog: &Catalog, build_id: Option<&str>) -> Result<Build> {
    let record = match build_id {
        Some(id) => catalog
            .record(id)
            .ok_or_else(|| EngineError::Config(format!("build {id:?} not found in catalog")))?,
        None => catalog
            .builds
            .iter()
            .find(|r| r.plan().is_ok())
            .ok_or_else(|| EngineError::Config("no supported build found in catalog".into()))?,
    };
    Ok(record.plan()?)
}

#[derive(Debug)]
pub struct EngineFailure {
    pub error: Box<EngineError>,
    pub log_path: Option<PathBuf>,
    pub irreversible: bool,
    pub failed_step: Option<String>,
}

impl EngineFailure {
    pub fn card_state(&self) -> CardErrorState {
        self.error.card_state(self.irreversible)
    }
}

pub fn run(
    request: &RunRequest,
    catalog: Option<&Catalog>,
    sink: &mut dyn ProgressSink,
    cancel: CancelToken,
) -> Result<RunSummary> {
    run_detailed(request, catalog, sink, cancel).map_err(|f| *f.error)
}

pub fn run_detailed(
    request: &RunRequest,
    catalog: Option<&Catalog>,
    sink: &mut dyn ProgressSink,
    cancel: CancelToken,
) -> std::result::Result<RunSummary, EngineFailure> {
    let work_dir = request.work_dir.clone().unwrap_or_else(default_cache_dir);
    let log_path = request
        .log_path
        .clone()
        .unwrap_or_else(|| timestamped_log_path(&default_log_dir()));
    let run_log = RunLog::create(log_path).ok();
    let mut progress = EngineProgress::new(sink, run_log, cancel);
    progress.log(format!("Starting {:?} on {}", request.mode, request.target.label()));

    match run_inner(request, catalog, &work_dir, &mut progress) {
        Ok(summary) => Ok(summary),
        Err(error) => {
            let failed_step = progress.active_step_id().map(str::to_owned);
            let irreversible = progress.is_irreversible();
            let log_path = progress.log_path();
            progress.log(format!("Error: {error}"));
            progress.fail_active_step();
            Err(EngineFailure {
                error: Box::new(error),
                log_path,
                irreversible,
                failed_step,
            })
        }
    }
}

fn run_inner(
    request: &RunRequest,
    provided_catalog: Option<&Catalog>,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<RunSummary> {
    match request.mode {
        Mode::Partition => {
            let image_used = request.image_used_override.unwrap_or(0);
            modes::run_partition(request, image_used, progress)
        }
        Mode::Install | Mode::PartitionAndInstall => {
            let loaded;
            let catalog = match provided_catalog {
                Some(c) => c,
                None => {
                    loaded = load_catalog(
                        request.local.catalog_path.as_deref(),
                        request.local.catalog_url.as_deref(),
                        request.local.release_tag.as_deref(),
                    )?;
                    &loaded
                }
            };
            let build = select_build(catalog, request.build_id.as_deref())?;
            if request.mode == Mode::Install {
                modes::run_install(request, catalog, &build, work_dir, progress)
            } else {
                modes::run_partition_and_install(request, catalog, &build, work_dir, progress)
            }
        }
    }
}
