// SPDX-License-Identifier: GPL-3.0-or-later
//! A role's delta, written into `<out>.partial` and moved to `<out>` only once the release's pins accept it. Dropped
//! uncommitted, it deletes what it wrote.

use std::fs;
use std::path::{Path, PathBuf};

use crate::Error;

pub struct Output {
    staging: PathBuf,
    out: PathBuf,
    committed: bool,
}

impl Output {
    pub fn create(out: &Path) -> Result<Self, Error> {
        if out.exists() {
            return Err(Error::Refused(format!("{} already exists", out.display())));
        }
        let mut name = out
            .file_name()
            .ok_or_else(|| Error::Refused(format!("{}: no directory name", out.display())))?
            .to_owned();
        name.push(".partial");
        let staging = out.with_file_name(name);
        if staging.exists() {
            fs::remove_dir_all(&staging)?;
        }
        fs::create_dir_all(&staging)?;
        Ok(Self {
            staging,
            out: out.to_owned(),
            committed: false,
        })
    }

    /// `rel` is `/`-separated, as the delta names it.
    pub fn write(&self, rel: &str, bytes: &[u8]) -> Result<(), Error> {
        let mut path = self.staging.clone();
        for part in rel.split('/') {
            if !storable(part) {
                return Err(Error::Refused(format!("{rel}: {part:?} can't be a file name here")));
            }
            path.push(part);
        }
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&path, bytes).map_err(|e| Error::Io(std::io::Error::new(e.kind(), format!("{rel}: {e}"))))
    }

    pub fn temp_dir(&self, name: &str) -> Result<TempDir, Error> {
        let path = self.staging.join(name);
        if path.exists() {
            fs::remove_dir_all(&path)?;
        }
        fs::create_dir_all(&path)?;
        Ok(TempDir(path))
    }

    pub fn commit(mut self) -> Result<PathBuf, Error> {
        fs::rename(&self.staging, &self.out)?;
        self.committed = true;
        Ok(self.out.clone())
    }
}

pub struct TempDir(PathBuf);

impl TempDir {
    pub fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

impl Drop for Output {
    fn drop(&mut self) {
        if !self.committed {
            let _ = fs::remove_dir_all(&self.staging);
        }
    }
}

fn storable(part: &str) -> bool {
    let reserved: &[char] = if cfg!(windows) {
        &['<', '>', ':', '"', '\\', '|', '?', '*']
    } else {
        &[]
    };
    !part.is_empty()
        && part != "."
        && part != ".."
        && !part
            .chars()
            .any(|c| c == '\0' || (cfg!(windows) && (c < ' ' || reserved.contains(&c))))
}
