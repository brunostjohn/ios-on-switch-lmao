// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;

use switch_catalog::Digest;
use switch_fetch::{Asset, Error, Url, content_length, fetch_release, parse_sums, verify_file};

use crate::server::{Server, noise, sha256};

fn sums_line(name: &str, data: &[u8]) -> String {
    format!("{}  {name}\n", hex::encode(sha256(data)))
}

#[tokio::test]
async fn fetches_assets_listed_in_sha256sums() {
    let (kernel, boot) = (noise(70_000, 21), noise(5_000, 22));
    let sums = sums_line("kernel.macho", &kernel) + &sums_line("boot.tar", &boot);
    let server = Server::start(&[
        ("r/SHA256SUMS", sums.as_bytes()),
        ("r/kernel.macho", &kernel),
        ("r/boot.tar", &boot),
    ]);
    let base = server.url("r/");
    let assets = [
        Asset {
            name: "kernel.macho".into(),
            size: kernel.len() as u64,
            sha256: Digest(sha256(&kernel)),
        },
        Asset {
            name: "boot.tar".into(),
            size: boot.len() as u64,
            sha256: Digest(sha256(&boot)),
        },
    ];
    let dir = tempfile::tempdir().unwrap();
    let client = reqwest::Client::new();
    fetch_release(&client, &base, &assets, dir.path(), |_, _| {})
        .await
        .unwrap();
    assert_eq!(fs::read(dir.path().join("kernel.macho")).unwrap(), kernel);
    assert_eq!(fs::read(dir.path().join("boot.tar")).unwrap(), boot);
    assert_eq!(
        content_length(&client, &server.url("r/kernel.macho")).await.unwrap(),
        70_000
    );
}

#[tokio::test]
async fn a_catalog_hash_sha256sums_disagrees_with_is_refused() {
    let kernel = noise(70_000, 23);
    let sums = sums_line("kernel.macho", &kernel);
    let server = Server::start(&[("r/SHA256SUMS", sums.as_bytes()), ("r/kernel.macho", &kernel)]);
    let mut pinned = sha256(&kernel);
    pinned[31] ^= 1;
    let assets = [Asset {
        name: "kernel.macho".into(),
        size: kernel.len() as u64,
        sha256: Digest(pinned),
    }];
    let dir = tempfile::tempdir().unwrap();
    let error = fetch_release(
        &reqwest::Client::new(),
        &server.url("r/"),
        &assets,
        dir.path(),
        |_, _| {},
    )
    .await
    .unwrap_err();
    assert!(matches!(error, Error::Release { .. }), "{error}");
    assert_eq!(server.requests().len(), 1);
}

/// The catalog and SHA256SUMS agree, and the release serves other bytes: nothing is kept.
#[tokio::test]
async fn an_asset_whose_bytes_arent_the_catalogs_is_refused() {
    let (kernel, served) = (noise(70_000, 41), noise(70_000, 43));
    let sums = sums_line("kernel.macho", &kernel);
    let server = Server::start(&[("r/SHA256SUMS", sums.as_bytes()), ("r/kernel.macho", &served)]);
    let assets = [Asset {
        name: "kernel.macho".into(),
        size: kernel.len() as u64,
        sha256: Digest(sha256(&kernel)),
    }];
    let dir = tempfile::tempdir().unwrap();
    let error = fetch_release(
        &reqwest::Client::new(),
        &server.url("r/"),
        &assets,
        dir.path(),
        |_, _| {},
    )
    .await
    .unwrap_err();
    assert!(matches!(error, Error::HashMismatch { .. }), "{error}");
    assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
}

#[test]
fn parses_sha256sums() {
    let a = "ab".repeat(32);
    let b = "CD".repeat(32);
    let sums = parse_sums(&format!("{a}  one.tar\n\n{b} *two.zip\n")).unwrap();
    assert_eq!(sums["one.tar"], [0xab; 32]);
    assert_eq!(sums["two.zip"], [0xcd; 32]);
    for bad in [
        format!("{a} one.tar"),
        format!("{a}  "),
        "zz  x".into(),
        format!("{a}  x\n{a}  x"),
    ] {
        assert!(matches!(parse_sums(&bad), Err(Error::Sums { .. })), "{bad:?}");
    }
}

#[test]
fn verifies_a_file() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("f");
    let data = noise(3 << 20, 24);
    fs::write(&path, &data).unwrap();
    verify_file(&path, &sha256(&data)).unwrap();
    let mut wrong = sha256(&data);
    wrong[0] ^= 1;
    assert!(matches!(verify_file(&path, &wrong), Err(Error::HashMismatch { .. })));
}

#[tokio::test]
async fn asset_names_stay_under_the_release() {
    let outside = noise(100, 25);
    let server = Server::start(&[
        ("r/SHA256SUMS", sums_line("../x", &outside).as_bytes()),
        ("x", &outside),
    ]);
    let assets = [Asset {
        name: "../x".into(),
        size: outside.len() as u64,
        sha256: Digest(sha256(&outside)),
    }];
    let dir = tempfile::tempdir().unwrap();
    let base: Url = server.url("r/");
    let error = fetch_release(&reqwest::Client::new(), &base, &assets, dir.path(), |_, _| {})
        .await
        .unwrap_err();
    assert!(matches!(error, Error::Url { .. }), "{error}");
}
