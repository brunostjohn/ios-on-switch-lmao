// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::Duration;

use switch_fetch::{Error, Span, checkpointed, fetch_span, fetch_span_with};

use crate::server::{Mode, Server, noise, sha256};

const LEN: usize = 1_000_000;
const OFFSET: u64 = 1_234;
const SPAN_LEN: u64 = 600_000;
// Not a multiple of SHA-256's 64-byte block, so a checkpoint carries a partly filled block.
const CHECKPOINT: u64 = 100_003;

struct Case {
    server: Server,
    data: Vec<u8>,
    dir: tempfile::TempDir,
    client: reqwest::Client,
}

impl Case {
    fn new() -> Case {
        let data = noise(LEN, 7);
        let server = Server::start(&[("member", &data)]);
        Case {
            server,
            data,
            dir: tempfile::tempdir().unwrap(),
            client: reqwest::Client::new(),
        }
    }

    fn span(&self, offset: u64, len: u64) -> Span {
        let sha256 = sha256(&self.data[offset as usize..(offset + len) as usize]);
        Span {
            url: self.server.url("member"),
            offset,
            len,
            sha256,
        }
    }

    fn dest(&self) -> PathBuf {
        self.dir.path().join("member.bin")
    }

    async fn fetch(&self, span: &Span) -> switch_fetch::Result<()> {
        fetch_span_with(&self.client, span, &self.dest(), CHECKPOINT, |_| {}).await
    }

    fn expect_done(&self, span: &Span) {
        let got = fs::read(self.dest()).unwrap();
        assert_eq!(got, &self.data[span.offset as usize..(span.offset + span.len) as usize]);
        assert!(!sibling(&self.dest(), ".part").exists());
        assert!(!sibling(&self.dest(), ".state").exists());
    }
}

fn sibling(dest: &Path, suffix: &str) -> PathBuf {
    let mut path = dest.as_os_str().to_owned();
    path.push(suffix);
    PathBuf::from(path)
}

#[tokio::test]
async fn fetches_a_span() {
    let case = Case::new();
    let span = case.span(OFFSET, SPAN_LEN);
    let mut seen = Vec::new();
    fetch_span_with(&case.client, &span, &case.dest(), CHECKPOINT, |n| seen.push(n))
        .await
        .unwrap();
    case.expect_done(&span);
    assert_eq!(seen.first(), Some(&0));
    assert_eq!(seen.last(), Some(&SPAN_LEN));
    assert_eq!(case.server.range_starts(), [OFFSET]);
}

#[tokio::test]
async fn present_dest_is_not_fetched_again() {
    let case = Case::new();
    let span = case.span(0, SPAN_LEN);
    fetch_span(&case.client, &span, &case.dest(), |_| {}).await.unwrap();
    fetch_span(&case.client, &span, &case.dest(), |_| {}).await.unwrap();
    assert_eq!(case.server.requests().len(), 1);
}

#[tokio::test]
async fn a_200_is_refused() {
    let case = Case::new();
    case.server.set_mode(Mode::IgnoreRange);
    let error = case.fetch(&case.span(OFFSET, SPAN_LEN)).await.unwrap_err();
    assert!(matches!(error, Error::NotPartial { status: 200, .. }), "{error}");
    assert!(!case.dest().exists());
}

#[tokio::test]
async fn a_wrong_content_range_is_refused() {
    let case = Case::new();
    case.server.set_mode(Mode::ShiftRange);
    let error = case.fetch(&case.span(OFFSET, SPAN_LEN)).await.unwrap_err();
    assert!(matches!(error, Error::ContentRange { .. }), "{error}");
}

#[tokio::test]
async fn a_cut_body_resumes_where_it_stopped() {
    let case = Case::new();
    let span = case.span(OFFSET, SPAN_LEN);
    case.server.set_mode(Mode::CutAfter(250_000));
    case.fetch(&span).await.unwrap_err();
    let held = checkpointed(&case.dest(), &span).await.unwrap();
    assert!(held >= 2 * CHECKPOINT && held <= 250_000, "{held}");
    case.server.set_mode(Mode::Normal);
    case.fetch(&span).await.unwrap();
    case.expect_done(&span);
    assert_eq!(case.server.range_starts(), [OFFSET, OFFSET + held]);
}

#[tokio::test]
async fn a_short_body_is_truncation_and_resumes() {
    let case = Case::new();
    let span = case.span(OFFSET, SPAN_LEN);
    case.server.set_mode(Mode::CloseAfter(150_000));
    let error = case.fetch(&span).await.unwrap_err();
    assert!(
        matches!(
            error,
            Error::BodyLength {
                got: 150_000,
                expected: SPAN_LEN,
                ..
            }
        ),
        "{error}"
    );
    assert_eq!(checkpointed(&case.dest(), &span).await, Some(150_000));
    case.server.set_mode(Mode::Normal);
    case.fetch(&span).await.unwrap();
    case.expect_done(&span);
    assert_eq!(case.server.range_starts(), [OFFSET, OFFSET + 150_000]);
}

#[tokio::test]
async fn an_abandoned_download_resumes_from_its_last_checkpoint() {
    let case = Case::new();
    let span = case.span(OFFSET, SPAN_LEN);
    case.server.set_mode(Mode::StallAfter(350_000));
    tokio::time::timeout(Duration::from_secs(2), case.fetch(&span))
        .await
        .unwrap_err();
    assert_eq!(checkpointed(&case.dest(), &span).await, Some(3 * CHECKPOINT));
    // The crash shape: bytes past the checkpoint that it does not count.
    fs::OpenOptions::new()
        .append(true)
        .open(sibling(&case.dest(), ".part"))
        .unwrap()
        .write_all(&[0xee; 4096])
        .unwrap();
    case.server.set_mode(Mode::Normal);
    case.fetch(&span).await.unwrap();
    case.expect_done(&span);
    assert_eq!(case.server.range_starts(), [OFFSET, OFFSET + 3 * CHECKPOINT]);
}

#[tokio::test]
async fn a_checkpoint_of_another_span_is_ignored() {
    let case = Case::new();
    case.server.set_mode(Mode::CloseAfter(150_000));
    case.fetch(&case.span(OFFSET, SPAN_LEN)).await.unwrap_err();
    case.server.set_mode(Mode::Normal);
    let other = case.span(OFFSET + 1, SPAN_LEN);
    assert_eq!(checkpointed(&case.dest(), &other).await, None);
    case.fetch(&other).await.unwrap();
    case.expect_done(&other);
    assert_eq!(case.server.range_starts(), [OFFSET, OFFSET + 1]);
}

#[tokio::test]
async fn a_hash_mismatch_deletes_the_download() {
    let case = Case::new();
    let mut span = case.span(OFFSET, SPAN_LEN);
    span.sha256[0] ^= 1;
    let error = case.fetch(&span).await.unwrap_err();
    assert!(matches!(error, Error::HashMismatch { .. }), "{error}");
    assert!(!case.dest().exists());
    assert!(!sibling(&case.dest(), ".part").exists());
    assert!(!sibling(&case.dest(), ".state").exists());
}

#[tokio::test]
async fn a_complete_checkpoint_finishes_without_a_request() {
    let case = Case::new();
    let span = case.span(OFFSET, SPAN_LEN);
    // A non-empty directory at dest makes the final rename fail after the last checkpoint.
    let blocker = case.dest().join("blocker");
    let result = fetch_span_with(&case.client, &span, &case.dest(), CHECKPOINT, |n| {
        if n == SPAN_LEN {
            fs::create_dir_all(case.dest()).unwrap();
            fs::write(&blocker, b"x").unwrap();
        }
    })
    .await;
    assert!(matches!(result, Err(Error::Io { .. })), "{result:?}");
    assert_eq!(checkpointed(&case.dest(), &span).await, Some(SPAN_LEN));
    fs::remove_dir_all(case.dest()).unwrap();
    case.server.set_mode(Mode::IgnoreRange);
    case.fetch(&span).await.unwrap();
    case.expect_done(&span);
    assert_eq!(case.server.requests().len(), 1);
}

#[tokio::test]
async fn an_empty_span_needs_no_request() {
    let case = Case::new();
    let span = case.span(OFFSET, 0);
    case.fetch(&span).await.unwrap();
    case.expect_done(&span);
    assert!(case.server.requests().is_empty());
}
