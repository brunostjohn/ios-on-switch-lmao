// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::BlockTarget;
use switch_apfs_write::{Content, Fill, StreamRef, Tree};

use crate::Error;
use crate::delta::Replacement;

const MAX_CHUNK: usize = 8 << 20;

/// Streams a regular file into its APFS extent during phase C, verifying exact length and optional SHA-256.
pub struct FileFill {
    path: PathBuf,
    size: u64,
    expected_sha256: Option<[u8; 32]>,
}

impl FileFill {
    pub fn new(path: PathBuf, size: u64, expected_sha256: Option<[u8; 32]>) -> Self {
        Self {
            path,
            size,
            expected_sha256,
        }
    }
}

impl Fill for FileFill {
    fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
        let mut file = File::open(&self.path)?;
        let cap = usize::try_from(self.size.next_multiple_of(BLOCK_SIZE as u64))
            .unwrap_or(MAX_CHUNK)
            .clamp(BLOCK_SIZE, MAX_CHUNK);
        let mut buf = vec![0u8; cap];
        let mut hasher = Sha256::new();
        let mut total = 0u64;
        let mut block = 0u64;
        let mut eof = false;

        while !eof {
            let mut filled = 0usize;
            while filled < buf.len() {
                match file.read(&mut buf[filled..])? {
                    0 => {
                        eof = true;
                        break;
                    }
                    n => filled += n,
                }
            }
            if filled == 0 {
                break;
            }
            total = total.checked_add(filled as u64).ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("{}: size overflow", self.path.display()),
                )
            })?;
            if total > self.size {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "{}: grew past planned {} bytes during write",
                        self.path.display(),
                        self.size
                    ),
                ));
            }
            if self.expected_sha256.is_some() {
                hasher.update(&buf[..filled]);
            }
            let padded = filled.next_multiple_of(BLOCK_SIZE);
            buf[filled..padded].fill(0);
            window.write_blocks(block, &buf[..padded])?;
            block += (padded / BLOCK_SIZE) as u64;
        }

        if total != self.size {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("{}: read {total} bytes, expected {}", self.path.display(), self.size),
            ));
        }
        if let Some(want) = self.expected_sha256 {
            let got = hasher.finalize();
            if got[..] != want[..] {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "{}: SHA-256 is {}, expected {}",
                        self.path.display(),
                        hex::encode(got),
                        hex::encode(want)
                    ),
                ));
            }
        }
        Ok(())
    }
}

/// Writes an in-memory buffer as a `Fill` so APFS allocates a single contiguous extent.
pub struct BytesFill {
    bytes: Arc<[u8]>,
}

impl BytesFill {
    pub fn new(bytes: Arc<[u8]>) -> Self {
        Self { bytes }
    }
}

impl Fill for BytesFill {
    fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
        let mut at = 0usize;
        let mut block = 0u64;
        let mut tail = Vec::new();
        while at < self.bytes.len() {
            let end = (at + MAX_CHUNK).min(self.bytes.len());
            let slice = &self.bytes[at..end];
            if slice.len().is_multiple_of(BLOCK_SIZE) {
                window.write_blocks(block, slice)?;
                block += (slice.len() / BLOCK_SIZE) as u64;
            } else {
                let padded = slice.len().next_multiple_of(BLOCK_SIZE);
                tail.clear();
                tail.reserve(padded);
                tail.extend_from_slice(slice);
                tail.resize(padded, 0);
                window.write_blocks(block, &tail)?;
                block += (padded / BLOCK_SIZE) as u64;
            }
            at = end;
        }
        Ok(())
    }
}

pub fn hash_file(path: &Path) -> Result<(u64, [u8; 32]), Error> {
    let mut file = File::open(path)?;
    let mut buf = vec![0u8; 1 << 20];
    let mut hasher = Sha256::new();
    let mut total = 0u64;
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        total += n as u64;
    }
    Ok((total, hasher.finalize().into()))
}

pub fn stream_from_replacement(
    tree: &mut Tree,
    path: &str,
    replacement: Replacement,
    expected_sha256: [u8; 32],
) -> Result<Option<StreamRef>, Error> {
    match replacement {
        Replacement::Bytes(bytes) => {
            let got: [u8; 32] = Sha256::digest(&bytes).into();
            if got != expected_sha256 {
                return Err(Error::OutputDigestMismatch {
                    path: path.into(),
                    got: hex::encode(got),
                    want: hex::encode(expected_sha256),
                });
            }
            if bytes.is_empty() {
                Ok(None)
            } else {
                Ok(Some(tree.add_bytes(bytes)))
            }
        }
        Replacement::File(file_path) => {
            let (size, got) = hash_file(&file_path)?;
            if got != expected_sha256 {
                return Err(Error::OutputDigestMismatch {
                    path: path.into(),
                    got: hex::encode(got),
                    want: hex::encode(expected_sha256),
                });
            }
            if size == 0 {
                Ok(None)
            } else {
                let fill = Arc::new(FileFill::new(file_path, size, Some(expected_sha256)));
                Ok(Some(tree.add_stream(size, Content::Fill(fill))))
            }
        }
    }
}
