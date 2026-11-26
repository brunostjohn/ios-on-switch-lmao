// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Read, Seek, SeekFrom};

use reqwest::Url;
use reqwest::blocking::Client;
use reqwest::header::RANGE;

use crate::error::{Error, Result, http};
use crate::range;

/// `Read + Seek` over `len` bytes of `url` from `base`, by range requests of at least `readahead` bytes. Blocking:
/// call it off any async runtime.
pub struct HttpFile {
    client: Client,
    url: Url,
    base: u64,
    len: u64,
    pos: u64,
    readahead: usize,
    buf: Vec<u8>,
    buf_pos: u64,
}

impl HttpFile {
    pub fn new(client: Client, url: Url, base: u64, len: u64, readahead: usize) -> Self {
        HttpFile {
            client,
            url,
            base,
            len,
            pos: 0,
            readahead: readahead.max(1),
            buf: Vec::new(),
            buf_pos: 0,
        }
    }

    /// The whole of `url`, its length taken from a one-byte range's Content-Range.
    pub fn open(client: Client, url: Url, readahead: usize) -> Result<Self> {
        let response = client
            .get(url.clone())
            .header(RANGE, range::header(0, 0))
            .send()
            .map_err(http(&url))?;
        let len = range::check_total(&url, response.status(), response.headers())?;
        Ok(HttpFile::new(client, url, 0, len, readahead))
    }

    pub fn len(&self) -> u64 {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn buffered(&self) -> bool {
        self.buf_pos <= self.pos && self.pos < self.buf_pos + self.buf.len() as u64
    }

    fn fill(&mut self, want: usize) -> Result<()> {
        let n = (want.max(self.readahead) as u64).min(self.len - self.pos);
        let (first, last) = (self.base + self.pos, self.base + self.pos + n - 1);
        let response = self
            .client
            .get(self.url.clone())
            .header(RANGE, range::header(first, last))
            .send();
        let response = response.map_err(http(&self.url))?;
        range::check(&self.url, response.status(), response.headers(), first, last)?;
        self.buf.clear();
        self.buf_pos = self.pos;
        let read = response.take(n).read_to_end(&mut self.buf);
        read.map_err(|source| Error::Body {
            url: self.url.to_string(),
            source,
        })?;
        if self.buf.len() as u64 != n {
            let got = self.buf.len() as u64;
            self.buf.clear();
            return Err(Error::BodyLength {
                url: self.url.to_string(),
                got,
                expected: n,
            });
        }
        Ok(())
    }
}

impl Read for HttpFile {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() || self.pos >= self.len {
            return Ok(0);
        }
        if !self.buffered() {
            self.fill(out.len()).map_err(io::Error::other)?;
        }
        let at = (self.pos - self.buf_pos) as usize;
        let n = out.len().min(self.buf.len() - at);
        out[..n].copy_from_slice(&self.buf[at..at + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for HttpFile {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let pos = match to {
            SeekFrom::Start(pos) => Some(pos),
            SeekFrom::End(delta) => self.len.checked_add_signed(delta),
            SeekFrom::Current(delta) => self.pos.checked_add_signed(delta),
        };
        self.pos = pos.ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "seek before the start"))?;
        Ok(self.pos)
    }
}
