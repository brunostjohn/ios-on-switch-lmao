// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{Read, Seek, SeekFrom, Write};
use std::ops::Range;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::thread::{self, ScopedJoinHandle};

use rayon::prelude::*;
use sha2::{Digest, Sha256};

use crate::index::Index;
use crate::{Error, Result};

const BATCH_BYTES: u64 = 32 << 20;

type Batch = Vec<Vec<u8>>;

/// Decrypts `archive` into `out` and returns the bytes written, once they hash to `content_sha256`.
///
/// One thread reads the stored segments in order, rayon decrypts and decompresses each batch of them, and another
/// thread writes and hashes the output in order. `progress` gets the output bytes written so far.
pub fn decrypt<R, W, P>(
    mut archive: R,
    key: &[u8; 32],
    mut out: W,
    content_sha256: &[u8; 32],
    progress: P,
) -> Result<u64>
where
    R: Read + Seek + Send,
    W: Write + Send,
    P: Fn(u64) + Sync,
{
    let index = Index::read(&mut archive, key)?;
    let batches = batches(&index, 2 * rayon::current_num_threads());
    let (stored_tx, stored_rx) = sync_channel(1);
    let (plain_tx, plain_rx) = sync_channel(1);
    // Each stage stops when the next one hangs up, so the first error ends all three without a deadlock.
    let (read, decoded, written) = thread::scope(|scope| {
        let reader = scope.spawn(|| read_batches(&mut archive, &index, &batches, stored_tx));
        let writer = scope.spawn(|| write_batches(&mut out, plain_rx, &progress));
        let decoded = decode_batches(&index, &batches, stored_rx, plain_tx);
        (join(reader), decoded, join(writer))
    });
    read?;
    decoded?;
    let (written, digest) = written?;
    if written != index.raw_size() {
        return Err(Error::Length {
            got: written,
            expected: index.raw_size(),
        });
    }
    if digest != *content_sha256 {
        return Err(Error::Content {
            got: hex::encode(digest),
            expected: hex::encode(content_sha256),
        });
    }
    Ok(written)
}

fn batches(index: &Index, most: usize) -> Vec<Range<usize>> {
    let mut batches = Vec::new();
    let (mut start, mut bytes) = (0, 0);
    for (i, segment) in index.segments().iter().enumerate() {
        if i > start && (i - start == most || bytes + u64::from(segment.stored_len) > BATCH_BYTES) {
            batches.push(start..i);
            (start, bytes) = (i, 0);
        }
        bytes += u64::from(segment.stored_len);
    }
    if start < index.segments().len() {
        batches.push(start..index.segments().len());
    }
    batches
}

fn read_batches<R: Read + Seek>(
    archive: &mut R,
    index: &Index,
    batches: &[Range<usize>],
    tx: SyncSender<Batch>,
) -> Result<()> {
    let mut position = None;
    for range in batches {
        let mut batch = Vec::with_capacity(range.len());
        for segment in &index.segments()[range.clone()] {
            if position != Some(segment.offset) {
                archive.seek(SeekFrom::Start(segment.offset)).map_err(Error::Read)?;
            }
            let mut stored = vec![0u8; segment.stored_len as usize];
            archive.read_exact(&mut stored).map_err(Error::Read)?;
            position = Some(segment.offset + u64::from(segment.stored_len));
            batch.push(stored);
        }
        if tx.send(batch).is_err() {
            return Ok(());
        }
    }
    index.verify_padding(archive)
}

fn decode_batches(index: &Index, batches: &[Range<usize>], rx: Receiver<Batch>, tx: SyncSender<Batch>) -> Result<()> {
    for (range, stored) in batches.iter().zip(rx) {
        let plain: Vec<Result<Vec<u8>>> = index.segments()[range.clone()]
            .par_iter()
            .zip(stored)
            .map(|(segment, stored)| index.decode(segment, stored))
            .collect();
        if tx.send(plain.into_iter().collect::<Result<_>>()?).is_err() {
            break;
        }
    }
    Ok(())
}

fn write_batches<W: Write>(out: &mut W, rx: Receiver<Batch>, progress: &impl Fn(u64)) -> Result<(u64, [u8; 32])> {
    let mut hash = Sha256::new();
    let mut written = 0u64;
    for batch in rx {
        for plain in batch {
            out.write_all(&plain).map_err(Error::Write)?;
            hash.update(&plain);
            written += plain.len() as u64;
        }
        progress(written);
    }
    out.flush().map_err(Error::Write)?;
    let mut digest = [0u8; 32];
    digest.copy_from_slice(&hash.finalize());
    Ok((written, digest))
}

fn join<T>(handle: ScopedJoinHandle<'_, T>) -> T {
    handle.join().unwrap_or_else(|panic| std::panic::resume_unwind(panic))
}
