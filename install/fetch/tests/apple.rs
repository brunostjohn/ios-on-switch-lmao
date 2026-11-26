// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Read;

use sha2::{Digest, Sha256};
use switch_catalog::{Catalog, Role};
use switch_fetch::{HttpFile, Span, Url, fetch_span};

/// Each build's device tree member: its Apple URL, stored range and stored SHA-256, from the catalog.
fn device_trees() -> Vec<(String, Span)> {
    let path = std::env::var("SWITCH_RECORDS").expect("SWITCH_RECORDS is set by the BUILD file");
    let catalog = Catalog::parse(&std::fs::read(path).unwrap()).unwrap();
    let spans: Vec<_> = catalog
        .builds
        .iter()
        .map(|record| {
            let build = record.plan().unwrap();
            let url = Url::parse(&record.ipsw.url).unwrap();
            (record.build.clone(), Span::member(&url, build.member(Role::DeviceTree)))
        })
        .collect();
    assert!(!spans.is_empty(), "the catalog has no builds");
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
