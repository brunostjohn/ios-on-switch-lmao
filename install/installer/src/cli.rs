// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;

use clap::{Parser, ValueEnum};
use switch_engine::{LocalInputs, Mode};
use switch_tui::{ALIGN_BYTES, Mode as TuiMode, parse_gib_to_bytes};

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum CliMode {
    Install,
    Partition,
    #[value(name = "partition+install", alias = "partition-and-install")]
    PartitionAndInstall,
}

impl From<CliMode> for Mode {
    fn from(value: CliMode) -> Self {
        match value {
            CliMode::Install => Mode::Install,
            CliMode::Partition => Mode::Partition,
            CliMode::PartitionAndInstall => Mode::PartitionAndInstall,
        }
    }
}

impl From<CliMode> for TuiMode {
    fn from(value: CliMode) -> Self {
        match value {
            CliMode::Install => TuiMode::Install,
            CliMode::Partition => TuiMode::Partition,
            CliMode::PartitionAndInstall => TuiMode::PartitionAndInstall,
        }
    }
}

#[derive(Clone, Debug, Parser)]
#[command(
    name = "switch_installer",
    about = "Install iPadOS onto a Nintendo Switch microSD card or disk image"
)]
pub struct Cli {
    #[arg(long, value_enum)]
    pub mode: Option<CliMode>,
    #[arg(long)]
    pub build: Option<String>,
    /// Target removable disk path or id (e.g. disk4, /dev/sdb, \\.\PhysicalDrive2).
    #[arg(long, conflicts_with = "image")]
    pub card: Option<String>,
    /// Local disk-image file target instead of a removable hardware card.
    #[arg(long, conflicts_with = "card")]
    pub image: Option<PathBuf>,
    /// GPT partition index for install mode (auto-selected when omitted).
    #[arg(long)]
    pub partition: Option<u32>,
    /// Size of the carved iOS partition (e.g. 64G, 64GiB, 48.5G, 65536M, or bytes).
    #[arg(long, value_parser = parse_size_bytes)]
    pub ios_size: Option<u64>,
    /// Resume an interrupted partition journal before continuing.
    #[arg(long)]
    pub resume: bool,
    /// Non-interactive confirmation; real cards also require --confirm <token>.
    #[arg(long)]
    pub yes: bool,
    /// Exact device confirmation token required for destructive non-interactive writes to a real card.
    #[arg(long)]
    pub confirm: Option<String>,
    /// Disable the interactive TUI and print plain-text progress.
    #[arg(long)]
    pub no_tui: bool,
    /// Validate inputs and print the execution plan without writing to any card or image.
    #[arg(long)]
    pub dry_run: bool,
    /// Leave the real card attached instead of ejecting after completion.
    #[arg(long)]
    pub no_eject: bool,
    /// Force-unmount busy volumes on the selected card.
    #[arg(long)]
    pub force_unmount: bool,
    /// Print discovered removable cards and exit.
    #[arg(long)]
    pub list_cards: bool,
    /// Print available builds from the catalog and exit.
    #[arg(long)]
    pub list_builds: bool,
    /// Release tag for remote catalog.json (defaults to latest release).
    #[arg(long)]
    pub release_tag: Option<String>,
    /// Override remote catalog.json URL.
    #[arg(long)]
    pub catalog_url: Option<String>,
    /// Local catalog.json file override (offline mode).
    #[arg(long)]
    pub catalog: Option<PathBuf>,
    /// Local directory containing downloaded release assets.
    #[arg(long)]
    pub assets_dir: Option<PathBuf>,
    /// Local boot asset tar override.
    #[arg(long)]
    pub boot_tar: Option<PathBuf>,
    /// Local prebuilt boot files directory override.
    #[arg(long)]
    pub boot_files_dir: Option<PathBuf>,
    /// Local IPSW zip file override.
    #[arg(long)]
    pub ipsw: Option<PathBuf>,
    /// Local OS APFS/UDIF image override.
    #[arg(long)]
    pub os_dmg: Option<PathBuf>,
    /// Local SystemOS cryptex image override.
    #[arg(long)]
    pub system_cryptex: Option<PathBuf>,
    /// Local AppOS cryptex image override.
    #[arg(long)]
    pub app_cryptex: Option<PathBuf>,
    /// Local overlay tree directory or .tar archive merged into Switch/overlay.
    #[arg(long)]
    pub overlay_dir: Option<PathBuf>,
    /// Optional built-in Installer app pin manifest (installer-bundle.json).
    #[arg(long)]
    pub overlay_builtin_pin: Option<PathBuf>,
    /// Optional Installer migration seed pin manifest (installer-seed.json).
    #[arg(long)]
    pub overlay_seed_pin: Option<PathBuf>,
    /// Local rootless bootstrap package directory or payload .tar archive.
    #[arg(long)]
    pub bootstrap_dir: Option<PathBuf>,
    /// Optional GPU bundle directory, gpu.stage manifest, or .tar archive merged onto Data.
    #[arg(long)]
    pub gpu_dir: Option<PathBuf>,
    /// Optional shader seed tree directory or .tar archive merged onto Data.
    #[arg(long)]
    pub shader_seed_dir: Option<PathBuf>,
    /// Optional checked byte-edit table for arm64e pretranslation.
    #[arg(long)]
    pub pretranslate_edits: Option<PathBuf>,
    /// Optional translator version string recorded in pretranslation manifests.
    #[arg(long)]
    pub translator_version: Option<String>,
    /// Optional precomputed OS pretranslation delta directory.
    #[arg(long)]
    pub pretranslated_os_dir: Option<PathBuf>,
    /// Optional precomputed SystemOS cryptex pretranslation delta directory.
    #[arg(long)]
    pub pretranslated_system_cryptex_dir: Option<PathBuf>,
    /// Optional precomputed AppOS cryptex pretranslation delta directory.
    #[arg(long)]
    pub pretranslated_app_cryptex_dir: Option<PathBuf>,
    /// Override cache/work directory.
    #[arg(long)]
    pub cache_dir: Option<PathBuf>,
    /// Override run log file path.
    #[arg(long)]
    pub log_file: Option<PathBuf>,
}

impl Cli {
    pub fn should_use_tui(&self, is_tty: bool) -> bool {
        is_tty && !self.no_tui && !self.yes && !self.dry_run && !self.list_cards && !self.list_builds
    }

    pub fn local_inputs(&self) -> LocalInputs {
        LocalInputs {
            catalog_path: self.catalog.clone(),
            catalog_url: self.catalog_url.clone(),
            release_tag: self.release_tag.clone(),
            assets_dir: self.assets_dir.clone(),
            boot_tar: self.boot_tar.clone(),
            boot_files_dir: self.boot_files_dir.clone(),
            ipsw_path: self.ipsw.clone(),
            os_dmg: self.os_dmg.clone(),
            system_cryptex: self.system_cryptex.clone(),
            app_cryptex: self.app_cryptex.clone(),
            overlay_dir: self.overlay_dir.clone(),
            overlay_builtin_pin: self.overlay_builtin_pin.clone(),
            overlay_seed_pin: self.overlay_seed_pin.clone(),
            bootstrap_dir: self.bootstrap_dir.clone(),
            gpu_dir: self.gpu_dir.clone(),
            shader_seed_dir: self.shader_seed_dir.clone(),
            pretranslate_edits: self.pretranslate_edits.clone(),
            translator_version: self.translator_version.clone(),
            pretranslated_os_dir: self.pretranslated_os_dir.clone(),
            pretranslated_system_cryptex_dir: self.pretranslated_system_cryptex_dir.clone(),
            pretranslated_app_cryptex_dir: self.pretranslated_app_cryptex_dir.clone(),
        }
    }
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum GuardError {
    #[error("non-interactive mode requires --mode <install|partition|partition+install>")]
    MissingMode,
    #[error("non-interactive mode requires --card <device> or --image <path>")]
    MissingTarget,
    #[error("refusing destructive card write without --yes and --confirm <token> (expected {expected:?})")]
    MissingConfirmation { expected: String },
    #[error("confirmation token {got:?} does not match target card token {expected:?}")]
    ConfirmMismatch { got: String, expected: String },
    #[error("iOS partition size {0} bytes is not aligned to 16 MiB")]
    UnalignedSize(u64),
}

pub fn validate_non_interactive(
    cli: &Cli,
    expected_confirm_token: Option<&str>,
    confirm_case_sensitive: bool,
) -> Result<Mode, GuardError> {
    let mode: Mode = cli.mode.ok_or(GuardError::MissingMode)?.into();
    if cli.card.is_none() && cli.image.is_none() {
        return Err(GuardError::MissingTarget);
    }
    if let Some(bytes) = cli.ios_size
        && (bytes == 0 || !bytes.is_multiple_of(ALIGN_BYTES))
    {
        return Err(GuardError::UnalignedSize(bytes));
    }
    if cli.dry_run || cli.image.is_some() {
        return Ok(mode);
    }
    let expected = expected_confirm_token.unwrap_or("");
    if !cli.yes {
        return Err(GuardError::MissingConfirmation {
            expected: expected.to_owned(),
        });
    }
    let Some(got) = cli.confirm.as_deref() else {
        return Err(GuardError::MissingConfirmation {
            expected: expected.to_owned(),
        });
    };
    let matches = if confirm_case_sensitive {
        got == expected
    } else {
        got.eq_ignore_ascii_case(expected)
    };
    if !matches {
        return Err(GuardError::ConfirmMismatch {
            got: got.to_owned(),
            expected: expected.to_owned(),
        });
    }
    Ok(mode)
}

pub fn parse_size_bytes(raw: &str) -> Result<u64, String> {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err("empty size".into());
    }
    let lower = trimmed.to_ascii_lowercase();
    const KIB: u64 = 1024;
    const MIB: u64 = 1024 * KIB;
    const GIB: u64 = 1024 * MIB;
    const TIB: u64 = 1024 * GIB;

    if let Some(num) = lower.strip_suffix("gib").or_else(|| lower.strip_suffix('g')) {
        return parse_gib_to_bytes(num.trim()).ok_or_else(|| format!("invalid GiB size: {raw:?}"));
    }
    if let Some(num) = lower.strip_suffix("tib").or_else(|| lower.strip_suffix('t')) {
        let val: f64 = num.trim().parse().map_err(|_| format!("invalid TiB size: {raw:?}"))?;
        if !val.is_finite() || val <= 0.0 {
            return Err(format!("invalid TiB size: {raw:?}"));
        }
        return Ok((val * TIB as f64).round() as u64);
    }
    if let Some(num) = lower.strip_suffix("mib").or_else(|| lower.strip_suffix('m')) {
        let val: u64 = num.trim().parse().map_err(|_| format!("invalid MiB size: {raw:?}"))?;
        return val.checked_mul(MIB).ok_or_else(|| format!("size overflow: {raw:?}"));
    }
    if let Some(num) = lower.strip_suffix("kib").or_else(|| lower.strip_suffix('k')) {
        let val: u64 = num.trim().parse().map_err(|_| format!("invalid KiB size: {raw:?}"))?;
        return val.checked_mul(KIB).ok_or_else(|| format!("size overflow: {raw:?}"));
    }
    lower
        .strip_suffix('b')
        .unwrap_or(&lower)
        .trim()
        .parse::<u64>()
        .map_err(|_| format!("invalid byte size: {raw:?}"))
}
