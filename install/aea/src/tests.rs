// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use sha2::{Digest, Sha256};
use tempfile::TempDir;

use crate::{Error, Index, Result, decrypt};

const KEY: [u8; 32] = [0x5a; 32];
const RUN: usize = 16 << 10;
// Nine 1 MiB segments and a 1,792-byte compressible tail, which LZFSE writes as an LZVN (bvxn) block.
const SIZE: usize = (9 << 20) + 1792;

struct Case {
    _dir: TempDir,
    input: Vec<u8>,
    archive: PathBuf,
}

// Alternate 16 KiB runs of incompressible and compressible bytes, so some segments are stored as they are.
fn mixed(len: usize) -> Vec<u8> {
    let text: Vec<u8> = (0..)
        .flat_map(|i| format!("line {i} of a compressible run\n").into_bytes())
        .take(RUN)
        .collect();
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut out = Vec::with_capacity(len + RUN);
    for run in 0.. {
        if out.len() >= len {
            break;
        }
        if run % 2 == 0 {
            out.extend_from_slice(&text);
            continue;
        }
        for _ in 0..RUN / 8 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            out.extend_from_slice(&state.to_le_bytes());
        }
    }
    out.truncate(len);
    out
}

fn encrypt(input: Vec<u8>, options: &[&str]) -> Case {
    let dir = match std::env::var_os("TEST_TMPDIR") {
        Some(root) => tempfile::tempdir_in(root),
        None => tempfile::tempdir(),
    }
    .unwrap();
    let plain = dir.path().join("plain");
    let archive = dir.path().join("archive.aea");
    fs::write(&plain, &input).unwrap();
    let status = Command::new("/usr/bin/aea")
        .args([
            "encrypt",
            "-profile",
            "1",
            "-key-value",
            &format!("hex:{}", hex::encode(KEY)),
        ])
        .args(options)
        .arg("-i")
        .arg(&plain)
        .arg("-o")
        .arg(&archive)
        .status()
        .unwrap();
    assert!(status.success(), "aea encrypt {options:?}: {status}");
    Case {
        _dir: dir,
        input,
        archive,
    }
}

fn sha256(data: &[u8]) -> [u8; 32] {
    let mut digest = [0u8; 32];
    digest.copy_from_slice(&Sha256::digest(data));
    digest
}

fn decrypted(archive: &Path, key: &[u8; 32], content_sha256: &[u8; 32]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    decrypt(File::open(archive).unwrap(), key, &mut out, content_sha256, |_| ())?;
    Ok(out)
}

fn round_trip(case: &Case) {
    let out = decrypted(&case.archive, &KEY, &sha256(&case.input)).unwrap();
    assert!(out == case.input, "the decrypted bytes differ from the input");
}

fn index(case: &Case) -> Index {
    Index::read(&mut File::open(&case.archive).unwrap(), &KEY).unwrap()
}

fn stored(archive: &Path, offset: u64, len: u32) -> Vec<u8> {
    fs::read(archive).unwrap()[offset as usize..][..len as usize].to_vec()
}

fn block_kinds(case: &Case, index: &Index) -> Vec<[u8; 4]> {
    index
        .segments()
        .iter()
        .filter(|s| s.stored_len != s.raw_len)
        .map(|s| {
            let plain = index.open(s, stored(&case.archive, s.offset, s.stored_len)).unwrap();
            plain[..4].try_into().unwrap()
        })
        .collect()
}

fn corrupt(case: &Case, at: u64) {
    let mut bytes = fs::read(&case.archive).unwrap();
    bytes[at as usize] ^= 1;
    fs::write(&case.archive, bytes).unwrap();
}

#[test]
fn small_segments_span_three_clusters_of_stored_lzfse_and_lzvn_segments() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    let index = index(&case);
    assert_eq!(index.clusters(), 3);
    let stored_raw = index.segments().iter().filter(|s| s.stored_len == s.raw_len).count();
    let kinds = block_kinds(&case, &index);
    assert!(stored_raw > 0, "no segment is stored uncompressed");
    assert!(kinds.contains(b"bvx2"), "no LZFSE segment");
    assert!(kinds.contains(b"bvxn"), "no LZVN segment");
    assert!(index.padding.len > 0);
    round_trip(&case);
}

#[test]
fn apple_parameters_round_trip() {
    let case = encrypt(mixed(SIZE), &[]);
    let index = index(&case);
    assert_eq!(index.segments().len(), 10);
    assert!(index.segments().iter().all(|s| s.stored_len < s.raw_len));
    round_trip(&case);
}

#[test]
fn an_archive_without_padding_round_trips() {
    let case = encrypt(mixed(SIZE), &["-b", "16k", "-padding", "none"]);
    assert_eq!(index(&case).padding.len, 0);
    round_trip(&case);
}

#[test]
fn an_uncompressed_archive_round_trips() {
    let case = encrypt(mixed(SIZE), &["-b", "16k", "-a", "none"]);
    assert!(index(&case).segments().iter().all(|s| s.stored_len == s.raw_len));
    round_trip(&case);
}

#[test]
fn an_empty_archive_round_trips() {
    let case = encrypt(Vec::new(), &[]);
    assert!(index(&case).segments().is_empty());
    round_trip(&case);
}

#[test]
fn a_damaged_segment_is_refused() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    let segment = index(&case).segments()[300].clone();
    corrupt(&case, segment.offset + u64::from(segment.stored_len) / 2);
    let error = decrypted(&case.archive, &KEY, &sha256(&case.input)).unwrap_err();
    assert!(
        matches!(
            error,
            Error::Segment {
                cluster: 1,
                index: 44,
                ..
            }
        ),
        "{error}"
    );
}

#[test]
fn a_damaged_cluster_header_is_refused() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    let first_of_cluster_2 = index(&case).segments()[512].offset;
    corrupt(&case, first_of_cluster_2 - 100);
    let error = Index::read(&mut File::open(&case.archive).unwrap(), &KEY)
        .err()
        .unwrap();
    assert!(
        matches!(error, Error::Authentication(ref what) if what.contains("cluster 2")),
        "{error}"
    );
}

#[test]
fn damaged_padding_is_refused() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    corrupt(&case, fs::metadata(&case.archive).unwrap().len() - 1);
    let error = decrypted(&case.archive, &KEY, &sha256(&case.input)).unwrap_err();
    assert!(
        matches!(error, Error::Authentication(ref what) if what == "the padding"),
        "{error}"
    );
}

#[test]
fn a_wrong_key_is_refused() {
    let case = encrypt(mixed(1 << 16), &[]);
    let error = decrypted(&case.archive, &[0xa5; 32], &sha256(&case.input)).unwrap_err();
    assert!(
        matches!(error, Error::Authentication(ref what) if what == "the root header"),
        "{error}"
    );
}

#[test]
fn a_truncated_archive_is_refused() {
    let case = encrypt(mixed(SIZE), &[]);
    let len = fs::metadata(&case.archive).unwrap().len();
    File::options()
        .write(true)
        .open(&case.archive)
        .unwrap()
        .set_len(len - 4096)
        .unwrap();
    let error = decrypted(&case.archive, &KEY, &sha256(&case.input)).unwrap_err();
    assert!(matches!(error, Error::Malformed(_)), "{error}");
}

#[test]
fn other_checksums_are_refused() {
    for checksum in ["none", "murmur64"] {
        let case = encrypt(mixed(1 << 16), &["-checksum", checksum]);
        let error = decrypted(&case.archive, &KEY, &sha256(&case.input)).unwrap_err();
        assert!(matches!(error, Error::Unsupported(_)), "{checksum}: {error}");
    }
}

#[test]
fn a_different_content_hash_is_refused() {
    let case = encrypt(mixed(SIZE), &[]);
    let error = decrypted(&case.archive, &KEY, &[0; 32]).unwrap_err();
    assert!(matches!(error, Error::Content { .. }), "{error}");
}

#[test]
fn keys_parse_as_aea_takes_them() {
    assert_eq!(crate::key_value(&format!("hex:{}", hex::encode(KEY))).unwrap(), KEY);
    assert_eq!(
        crate::key_value("base64:WlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlo=").unwrap(),
        KEY
    );
    for bad in ["WlpaWlpa", "hex:5a5a", "base64:not base64", &hex::encode(KEY)] {
        assert!(matches!(crate::key_value(bad), Err(Error::Key)), "{bad}");
    }
}

struct FailingWriter(usize);

impl Write for FailingWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        if self.0 < data.len() {
            return Err(io::Error::other("the disk is full"));
        }
        self.0 -= data.len();
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[test]
fn a_failing_writer_ends_the_decryption() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    let file = File::open(&case.archive).unwrap();
    let error = decrypt(file, &KEY, FailingWriter(1 << 20), &sha256(&case.input), |_| ()).unwrap_err();
    assert!(matches!(error, Error::Write(_)), "{error}");
}

#[test]
fn progress_reaches_the_raw_size() {
    let case = encrypt(mixed(SIZE), &["-b", "16k"]);
    let last = std::sync::atomic::AtomicU64::new(0);
    let file = File::open(&case.archive).unwrap();
    let progress = |done| last.store(done, std::sync::atomic::Ordering::Relaxed);
    decrypt(file, &KEY, io::sink(), &sha256(&case.input), progress).unwrap();
    assert_eq!(last.into_inner(), SIZE as u64);
}
