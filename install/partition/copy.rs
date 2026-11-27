// SPDX-License-Identifier: GPL-3.0-or-later
//! Moving cluster data: long reads and writes through one page-aligned buffer, timed for the report.

use std::time::{Duration, Instant};

use switch_card::{Device, SECTOR};

use crate::cache::MAX_REQUEST;
use crate::error::Result;

const ALIGN: usize = 4096;

pub struct Copier {
    buf: Vec<u8>,
    pub bytes: u64,
    pub time: Duration,
}

impl Default for Copier {
    fn default() -> Self {
        Copier {
            buf: vec![0; MAX_REQUEST as usize * SECTOR + ALIGN],
            bytes: 0,
            time: Duration::ZERO,
        }
    }
}

impl Copier {
    /// Copies each (source LBA, destination LBA, sectors), then makes the copies durable. Sources never overlap a
    /// destination: one lies past the cut, the other below it.
    pub fn copy<D: Device>(&mut self, dev: &mut D, ops: &[(u64, u64, u64)]) -> Result<()> {
        let started = Instant::now();
        let skip = self.buf.as_ptr().align_offset(ALIGN);
        for &(src, dst, sectors) in ops {
            let mut done = 0;
            while done < sectors {
                let n = (sectors - done).min(MAX_REQUEST);
                let chunk = &mut self.buf[skip..skip + n as usize * SECTOR];
                dev.read_sectors(src + done, chunk)?;
                dev.write_sectors(dst + done, chunk)?;
                done += n;
            }
            self.bytes += sectors * SECTOR as u64;
        }
        if !ops.is_empty() {
            dev.barrier()?;
        }
        self.time += started.elapsed();
        Ok(())
    }

    /// Bytes a second over every copy so far, barriers included.
    pub fn throughput(&self) -> f64 {
        let secs = self.time.as_secs_f64();
        if secs > 0.0 { self.bytes as f64 / secs } else { 0.0 }
    }
}
