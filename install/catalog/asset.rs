// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the catalog and its assets are published, and an asset's bytes checked against the catalog as they are read.

use std::io::{self, ErrorKind, Read};

use sha2::{Digest as _, Sha256};

use crate::record::{Asset, Catalog};
use crate::{Error, REPO};

pub const CATALOG_NAME: &str = "catalog.json";

/// The catalog of the release `tag`, or of the latest release.
pub fn catalog_url(tag: Option<&str>) -> String {
    match tag {
        Some(tag) => format!("https://github.com/{REPO}/releases/download/{tag}/{CATALOG_NAME}"),
        None => format!("https://github.com/{REPO}/releases/latest/download/{CATALOG_NAME}"),
    }
}

impl Catalog {
    /// The release the assets are under, ending in `/`.
    pub fn assets_url(&self) -> String {
        format!("https://github.com/{REPO}/releases/download/{}/", self.release)
    }
}

impl Asset {
    /// `reader`, hashed as it is read. [`Checked::finish`] reads the rest and refuses bytes that aren't this asset's.
    pub fn checked<R: Read>(&self, reader: R) -> Checked<'_, R> {
        Checked {
            asset: self,
            inner: reader,
            hasher: Sha256::new(),
            len: 0,
        }
    }

    /// Reads `reader` to its end and refuses it unless it is this asset's bytes.
    pub fn check(&self, reader: impl Read) -> Result<(), Error> {
        self.checked(reader).finish()
    }
}

pub struct Checked<'a, R> {
    asset: &'a Asset,
    inner: R,
    hasher: Sha256,
    len: u64,
}

impl<R: Read> Read for Checked<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.hasher.update(&buf[..n]);
        self.len += n as u64;
        Ok(n)
    }
}

impl<R: Read> Checked<'_, R> {
    pub fn finish(mut self) -> Result<(), Error> {
        let mut rest = vec![0; 1 << 20];
        loop {
            match self.read(&mut rest) {
                Ok(0) => break,
                Ok(_) => {}
                Err(e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return Err(Error::Read(self.asset.name.clone(), e)),
            }
        }
        let got: [u8; 32] = self.hasher.finalize().into();
        if self.len != self.asset.size || got != self.asset.sha256.0 {
            return Err(Error::AssetMismatch {
                name: self.asset.name.clone(),
                got_size: self.len,
                got: hex::encode(got),
                size: self.asset.size,
                sha256: self.asset.sha256.to_string(),
            });
        }
        Ok(())
    }
}
