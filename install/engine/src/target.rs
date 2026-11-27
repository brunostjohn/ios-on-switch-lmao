// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{File, OpenOptions};
use std::io;
use std::path::{Path, PathBuf};

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_card::{Device, SECTOR};
use switch_disk::{Disk, OpenOptions as DiskOpenOptions, RawDisk};

use crate::error::{EngineError, Result};
use crate::progress::EngineProgress;

#[derive(Clone, Debug)]
pub enum TargetSpec {
    Disk {
        disk: Disk,
        options: DiskOpenOptions,
        eject: bool,
    },
    ImageFile {
        path: PathBuf,
        sector_size: u32,
    },
}

impl TargetSpec {
    pub fn image_file(path: impl Into<PathBuf>) -> Self {
        Self::ImageFile {
            path: path.into(),
            sector_size: SECTOR as u32,
        }
    }

    pub fn label(&self) -> String {
        match self {
            Self::Disk { disk, .. } => {
                if disk.model.is_empty() {
                    disk.path.display().to_string()
                } else {
                    format!("{} ({})", disk.path.display(), disk.model)
                }
            }
            Self::ImageFile { path, .. } => path.display().to_string(),
        }
    }

    pub fn is_real_disk(&self) -> bool {
        matches!(self, Self::Disk { .. })
    }
}

enum Backend {
    Raw { disk: Box<RawDisk>, eject: bool },
    File { file: File, path: PathBuf },
}

pub struct OpenedTarget {
    backend: Backend,
    size: u64,
    sector_size: u32,
    label: String,
}

impl OpenedTarget {
    pub fn open(spec: &TargetSpec) -> Result<Self> {
        let label = spec.label();
        match spec {
            TargetSpec::Disk { disk, options, eject } => {
                let raw = switch_disk::open(disk, options)?;
                let size = raw.size();
                let sector_size = raw.logical_sector();
                Ok(Self {
                    backend: Backend::Raw {
                        disk: Box::new(raw),
                        eject: *eject,
                    },
                    size,
                    sector_size,
                    label,
                })
            }
            TargetSpec::ImageFile { path, sector_size } => {
                let file = OpenOptions::new()
                    .read(true)
                    .write(true)
                    .open(path)
                    .map_err(|e| EngineError::io_path(path, e))?;
                let size = file.metadata().map_err(|e| EngineError::io_path(path, e))?.len();
                Ok(Self {
                    backend: Backend::File {
                        file,
                        path: path.clone(),
                    },
                    size,
                    sector_size: *sector_size,
                    label,
                })
            }
        }
    }

    pub fn open_image_readonly(path: &Path, sector_size: u32) -> Result<Self> {
        let file = File::open(path).map_err(|e| EngineError::io_path(path, e))?;
        let size = file.metadata().map_err(|e| EngineError::io_path(path, e))?.len();
        Ok(Self {
            backend: Backend::File {
                file,
                path: path.to_path_buf(),
            },
            size,
            sector_size,
            label: path.display().to_string(),
        })
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub fn size_bytes(&self) -> u64 {
        self.size
    }

    pub fn is_real_disk(&self) -> bool {
        matches!(self.backend, Backend::Raw { .. })
    }

    pub fn read_at(&self, offset: u64, buf: &mut [u8]) -> io::Result<()> {
        check_bounds(offset, buf.len(), self.size)?;
        match &self.backend {
            Backend::Raw { disk, .. } => disk.read_at(offset, buf).map_err(io::Error::other),
            Backend::File { file, .. } => positional::read_exact_at(file, buf, offset),
        }
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> io::Result<()> {
        check_bounds(offset, data.len(), self.size)?;
        match &self.backend {
            Backend::Raw { disk, .. } => disk.write_at(offset, data).map_err(io::Error::other),
            Backend::File { file, .. } => positional::write_all_at(file, data, offset),
        }
    }

    pub fn flush(&self) -> io::Result<()> {
        match &self.backend {
            Backend::Raw { disk, .. } => disk.flush().map_err(io::Error::other),
            Backend::File { file, .. } => file.sync_data(),
        }
    }

    pub fn partition_window(&self, first_lba: u64, sectors: u64) -> Result<PartitionWindow<'_>> {
        let sector = u64::from(self.sector_size);
        let offset = first_lba
            .checked_mul(sector)
            .ok_or_else(|| EngineError::Config("partition offset overflow".into()))?;
        let len = sectors
            .checked_mul(sector)
            .ok_or_else(|| EngineError::Config("partition length overflow".into()))?;
        if offset.checked_add(len).is_none_or(|end| end > self.size) {
            return Err(EngineError::Config(format!(
                "partition {first_lba}+{sectors} exceeds target size {}",
                self.size
            )));
        }
        Ok(PartitionWindow {
            target: self,
            offset,
            len,
        })
    }

    /// Flushes and ejects a real card (unless `--no-eject`), preventing macOS DiskArbitration from mounting new volumes.
    pub fn finish(self) -> Result<bool> {
        match self.backend {
            Backend::Raw { disk, eject } => {
                disk.flush()?;
                if eject {
                    disk.eject()?;
                    Ok(true)
                } else {
                    disk.release()?;
                    Ok(false)
                }
            }
            Backend::File { file, path } => {
                file.sync_data().map_err(|e| EngineError::io_path(&path, e))?;
                Ok(false)
            }
        }
    }
}

impl Device for OpenedTarget {
    fn sector_size(&self) -> u32 {
        self.sector_size
    }

    fn sector_count(&self) -> u64 {
        self.size / u64::from(self.sector_size)
    }

    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        let sector = u64::from(self.sector_size);
        if !(buf.len() as u64).is_multiple_of(sector) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "unaligned sector read length",
            ));
        }
        let offset = lba
            .checked_mul(sector)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "sector offset overflow"))?;
        self.read_at(offset, buf)
    }

    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        let sector = u64::from(self.sector_size);
        if !(buf.len() as u64).is_multiple_of(sector) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "unaligned sector write length",
            ));
        }
        let offset = lba
            .checked_mul(sector)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "sector offset overflow"))?;
        self.write_at(offset, buf)
    }

    fn barrier(&mut self) -> io::Result<()> {
        self.flush()
    }
}

pub struct PartitionWindow<'a> {
    target: &'a OpenedTarget,
    offset: u64,
    len: u64,
}

impl PartitionWindow<'_> {
    pub fn blocks(&self) -> u64 {
        self.len / BLOCK_SIZE as u64
    }

    pub fn byte_len(&self) -> u64 {
        self.len
    }

    fn byte_offset(&self, paddr: u64, data_len: usize) -> io::Result<u64> {
        let rel = paddr
            .checked_mul(BLOCK_SIZE as u64)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "block offset overflow"))?;
        let end = rel
            .checked_add(data_len as u64)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "block transfer overflow"))?;
        if end > self.len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("APFS transfer {rel}..{end} exceeds partition bytes {}", self.len),
            ));
        }
        Ok(self.offset + rel)
    }
}

impl BlockSource for PartitionWindow<'_> {
    fn read_blocks(&self, paddr: u64, out: &mut [u8]) -> io::Result<()> {
        let at = self.byte_offset(paddr, out.len())?;
        self.target.read_at(at, out)
    }
}

impl BlockTarget for PartitionWindow<'_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        let at = self.byte_offset(paddr, data.len())?;
        self.target.write_at(at, data)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.target.flush()
    }
}

pub struct ProgressBlockTarget<'a, 'p> {
    inner: PartitionWindow<'a>,
    progress: &'a mut EngineProgress<'p>,
    step_id: String,
    written: u64,
    total: u64,
}

impl<'a, 'p> ProgressBlockTarget<'a, 'p> {
    pub fn new(
        inner: PartitionWindow<'a>,
        progress: &'a mut EngineProgress<'p>,
        step_id: impl Into<String>,
        total: u64,
    ) -> Self {
        Self {
            inner,
            progress,
            step_id: step_id.into(),
            written: 0,
            total,
        }
    }

    pub fn written_bytes(&self) -> u64 {
        self.written
    }
}

impl BlockTarget for ProgressBlockTarget<'_, '_> {
    fn write_blocks(&mut self, paddr: u64, data: &[u8]) -> io::Result<()> {
        self.inner.write_blocks(paddr, data)?;
        self.written = self.written.saturating_add(data.len() as u64);
        self.progress.step_progress(
            &self.step_id,
            self.written,
            Some(self.total.max(self.written)),
            Some("Streaming APFS blocks".to_owned()),
            false,
        );
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()?;
        self.progress.step_progress(
            &self.step_id,
            self.written,
            Some(self.total.max(self.written)),
            Some("Flushing card cache".to_owned()),
            true,
        );
        Ok(())
    }
}

fn check_bounds(offset: u64, len: usize, total: u64) -> io::Result<()> {
    match offset.checked_add(len as u64) {
        Some(end) if end <= total => Ok(()),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("transfer {offset}+{len} outside target size {total}"),
        )),
    }
}

#[cfg(unix)]
mod positional {
    use std::fs::File;
    use std::io;
    use std::os::unix::fs::FileExt;

    pub fn read_exact_at(file: &File, buf: &mut [u8], offset: u64) -> io::Result<()> {
        file.read_exact_at(buf, offset)
    }

    pub fn write_all_at(file: &File, buf: &[u8], offset: u64) -> io::Result<()> {
        file.write_all_at(buf, offset)
    }
}

#[cfg(windows)]
mod positional {
    use std::fs::File;
    use std::io;
    use std::os::windows::fs::FileExt;

    pub fn read_exact_at(file: &File, mut buf: &mut [u8], mut offset: u64) -> io::Result<()> {
        while !buf.is_empty() {
            match file.seek_read(buf, offset)? {
                0 => return Err(io::ErrorKind::UnexpectedEof.into()),
                n => {
                    buf = &mut buf[n..];
                    offset += n as u64;
                }
            }
        }
        Ok(())
    }

    pub fn write_all_at(file: &File, mut buf: &[u8], mut offset: u64) -> io::Result<()> {
        while !buf.is_empty() {
            match file.seek_write(buf, offset)? {
                0 => return Err(io::ErrorKind::WriteZero.into()),
                n => {
                    buf = &buf[n..];
                    offset += n as u64;
                }
            }
        }
        Ok(())
    }
}
