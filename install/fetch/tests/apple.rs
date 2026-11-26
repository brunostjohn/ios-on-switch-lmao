// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Read;

use sha2::{Digest, Sha256};
use switch_fetch::{HttpFile, Span, Url, fetch_span};

/// Each pinned kind's device tree member: its Apple URL, stored range and stored SHA-256, from pins.json.
fn device_trees() -> Vec<(String, Span)> {
    let runfiles = runfiles::Runfiles::create().unwrap();
    let path = runfiles::rlocation!(runfiles, std::env::var("SWITCH_PINS").unwrap()).unwrap();
    let pins: serde_json::Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    let mut spans = Vec::new();
    for (kind, ipsw) in pins.as_object().unwrap() {
        let Some(member) = ipsw.pointer("/members/device_tree") else {
            continue;
        };
        let mut sha256 = [0u8; 32];
        hex::decode_to_slice(member["sha256"].as_str().unwrap(), &mut sha256).unwrap();
        let span = Span {
            url: Url::parse(ipsw["url"].as_str().unwrap()).unwrap(),
            offset: member["offset"].as_u64().unwrap(),
            len: member["stored_size"].as_u64().unwrap(),
            sha256,
        };
        spans.push((kind.clone(), span));
    }
    assert!(!spans.is_empty(), "pins.json pins no device tree");
    spans
}

#[tokio::test]
async fn device_trees_match_their_pins() {
    let client = reqwest::Client::new();
    let dir = tempfile::tempdir().unwrap();
    for (kind, span) in device_trees() {
        let dest = dir.path().join(&kind);
        fetch_span(&client, &span, &dest, |_| {})
            .await
            .unwrap_or_else(|e| panic!("{kind}: {e}"));
        assert_eq!(std::fs::metadata(&dest).unwrap().len(), span.len, "{kind}");
    }
}

#[test]
fn device_trees_read_through_http_file_match_their_pins() {
    for (kind, span) in device_trees() {
        let client = reqwest::blocking::Client::new();
        let mut file = HttpFile::new(client, span.url.clone(), span.offset, span.len, 1 << 20);
        let mut stored = Vec::new();
        file.read_to_end(&mut stored).unwrap();
        assert_eq!(<[u8; 32]>::from(Sha256::digest(&stored)), span.sha256, "{kind}");
    }
}
