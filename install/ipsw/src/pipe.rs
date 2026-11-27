// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, Write};
use std::mem;
use std::sync::mpsc::{Receiver, SyncSender, channel, sync_channel};
use std::thread;

use sha2::{Digest, Sha256};

use crate::error::{Error, Result};

const CHUNK: usize = 4 << 20;
const DEPTH: usize = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Written {
    pub len: u64,
    pub sha256: [u8; 32],
}

/// The producer's end: bytes go into fixed chunks that a second thread writes and hashes.
pub(crate) struct Sink {
    buf: Vec<u8>,
    filled: usize,
    full: SyncSender<(Vec<u8>, usize)>,
    free: Receiver<Vec<u8>>,
}

impl Sink {
    /// Lets `f` produce straight into the current chunk's free space, which is never empty. `f` returns how many bytes
    /// it wrote and its own result.
    pub fn fill<T>(&mut self, f: impl FnOnce(&mut [u8]) -> io::Result<(usize, T)>) -> io::Result<T> {
        if self.filled == self.buf.len() {
            self.send()?;
        }
        let (n, value) = f(&mut self.buf[self.filled..])?;
        assert!(n <= self.buf.len() - self.filled);
        self.filled += n;
        Ok(value)
    }

    fn send(&mut self) -> io::Result<()> {
        if self.filled == 0 {
            return Ok(());
        }
        let next = self.free.try_recv().unwrap_or_else(|_| vec![0u8; CHUNK]);
        let full = mem::replace(&mut self.buf, next);
        self.full
            .send((full, mem::take(&mut self.filled)))
            .map_err(|_| io::ErrorKind::BrokenPipe.into())
    }
}

impl Write for Sink {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.fill(|spare| {
            let n = data.len().min(spare.len());
            spare[..n].copy_from_slice(&data[..n]);
            Ok((n, n))
        })
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Runs `produce` on this thread while a second one writes what it produced to `out` and hashes it.
pub(crate) fn hashed<W: Write + Send, T>(
    out: &mut W,
    produce: impl FnOnce(&mut Sink) -> Result<T>,
) -> Result<(T, Written)> {
    let (full, full_rx) = sync_channel::<(Vec<u8>, usize)>(DEPTH);
    let (free_tx, free) = channel();
    thread::scope(|scope| {
        let consumer = scope.spawn(move || -> io::Result<Written> {
            let (mut hasher, mut len) = (Sha256::new(), 0u64);
            for (buf, n) in full_rx {
                out.write_all(&buf[..n])?;
                hasher.update(&buf[..n]);
                len += n as u64;
                let _ = free_tx.send(buf);
            }
            out.flush()?;
            Ok(Written {
                len,
                sha256: hasher.finalize().into(),
            })
        });
        let mut sink = Sink {
            buf: vec![0u8; CHUNK],
            filled: 0,
            full,
            free,
        };
        let produced = produce(&mut sink).and_then(|value| sink.send().map(|()| value).map_err(Error::from));
        drop(sink);
        let consumed = consumer.join().unwrap_or_else(|panic| std::panic::resume_unwind(panic));
        // A failed write stops the consumer, so the producer then sees a broken pipe: the write is the cause.
        match (produced, consumed) {
            (_, Err(error)) => Err(Error::Write(error)),
            (Err(error), Ok(_)) => Err(error),
            (Ok(value), Ok(written)) => Ok((value, written)),
        }
    })
}
