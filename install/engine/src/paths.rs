// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::error::{EngineError, Result};

pub const APP_DATA_DIR_NAME: &str = "switch-ios";

pub fn default_cache_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = env::var_os("HOME") {
            return PathBuf::from(home)
                .join("Library")
                .join("Application Support")
                .join(APP_DATA_DIR_NAME)
                .join("cache");
        }
    }
    #[cfg(target_os = "windows")]
    {
        if let Some(local) = env::var_os("LOCALAPPDATA") {
            return PathBuf::from(local).join(APP_DATA_DIR_NAME).join("cache");
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(xdg) = env::var_os("XDG_CACHE_HOME")
            && !xdg.is_empty()
        {
            return PathBuf::from(xdg).join(APP_DATA_DIR_NAME);
        }
        if let Some(home) = env::var_os("HOME") {
            return PathBuf::from(home).join(".cache").join(APP_DATA_DIR_NAME);
        }
    }
    env::temp_dir().join(APP_DATA_DIR_NAME).join("cache")
}

pub fn default_log_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = env::var_os("HOME") {
            return PathBuf::from(home)
                .join("Library")
                .join("Application Support")
                .join(APP_DATA_DIR_NAME)
                .join("logs");
        }
    }
    #[cfg(target_os = "windows")]
    {
        if let Some(local) = env::var_os("LOCALAPPDATA") {
            return PathBuf::from(local).join(APP_DATA_DIR_NAME).join("logs");
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(xdg) = env::var_os("XDG_DATA_HOME")
            && !xdg.is_empty()
        {
            return PathBuf::from(xdg).join(APP_DATA_DIR_NAME).join("logs");
        }
        if let Some(home) = env::var_os("HOME") {
            return PathBuf::from(home)
                .join(".local")
                .join("share")
                .join(APP_DATA_DIR_NAME)
                .join("logs");
        }
    }
    env::temp_dir().join(APP_DATA_DIR_NAME).join("logs")
}

pub fn timestamped_log_path(log_dir: &Path) -> PathBuf {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let pid = std::process::id();
    log_dir.join(format!("switch-installer-{secs}-{pid}.log"))
}

pub struct RunLog {
    path: PathBuf,
    writer: BufWriter<File>,
}

impl RunLog {
    pub fn create(path: PathBuf) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|e| EngineError::io_path(parent, e))?;
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .map_err(|e| EngineError::io_path(&path, e))?;
        Ok(Self {
            path,
            writer: BufWriter::new(file),
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn write_line(&mut self, line: &str) {
        let secs = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let _ = writeln!(self.writer, "[{secs}] {line}");
        let _ = self.writer.flush();
    }
}
