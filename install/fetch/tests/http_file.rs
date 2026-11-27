// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{ErrorKind, Read, Seek, SeekFrom};

use switch_fetch::HttpFile;

use crate::server::{Mode, Server, noise};

const LEN: usize = 300_000;
const READAHEAD: usize = 16 << 10;

fn read_at(file: &mut HttpFile, at: u64, len: usize) -> Vec<u8> {
    file.seek(SeekFrom::Start(at)).unwrap();
    let mut out = vec![0u8; len];
    file.read_exact(&mut out).unwrap();
    out
}

#[test]
fn reads_and_seeks_match_the_file() {
    let data = noise(LEN, 11);
    let server = Server::start(&[("file", &data)]);
    let mut file = HttpFile::open(reqwest::blocking::Client::new(), server.url("file"), READAHEAD).unwrap();
    assert_eq!(file.len(), LEN as u64);
    for (at, len) in [(0, 10), (5, 100), (299_990, 10), (123_456, 70_000), (1, 1), (65_535, 2)] {
        assert_eq!(
            read_at(&mut file, at, len),
            &data[at as usize..at as usize + len],
            "{at}+{len}"
        );
    }
    assert_eq!(file.seek(SeekFrom::End(-4)).unwrap(), LEN as u64 - 4);
    let mut tail = Vec::new();
    file.read_to_end(&mut tail).unwrap();
    assert_eq!(tail, &data[LEN - 4..]);
    assert_eq!(file.read(&mut [0u8; 8]).unwrap(), 0);
    assert_eq!(
        file.seek(SeekFrom::Current(-(LEN as i64) - 1)).unwrap_err().kind(),
        ErrorKind::InvalidInput
    );
}

#[test]
fn sequential_reads_come_from_the_readahead() {
    let data = noise(LEN, 12);
    let server = Server::start(&[("file", &data)]);
    let mut file = HttpFile::new(
        reqwest::blocking::Client::new(),
        server.url("file"),
        0,
        LEN as u64,
        READAHEAD,
    );
    let mut all = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        let n = file.read(&mut chunk).unwrap();
        if n == 0 {
            break;
        }
        all.extend_from_slice(&chunk[..n]);
    }
    assert_eq!(all, data);
    assert_eq!(server.requests().len(), LEN.div_ceil(READAHEAD));
}

#[test]
fn a_view_reads_only_its_span() {
    let data = noise(LEN, 13);
    let server = Server::start(&[("file", &data)]);
    let mut file = HttpFile::new(
        reqwest::blocking::Client::new(),
        server.url("file"),
        1_000,
        5_000,
        READAHEAD,
    );
    let mut all = Vec::new();
    file.read_to_end(&mut all).unwrap();
    assert_eq!(all, &data[1_000..6_000]);
    assert_eq!(server.requests()[0].range, Some((1_000, Some(5_999))));
}

#[test]
fn a_200_is_an_error() {
    let data = noise(LEN, 14);
    let server = Server::start(&[("file", &data)]);
    server.set_mode(Mode::IgnoreRange);
    let mut file = HttpFile::new(
        reqwest::blocking::Client::new(),
        server.url("file"),
        0,
        LEN as u64,
        READAHEAD,
    );
    let error = file.read(&mut [0u8; 16]).unwrap_err();
    assert!(error.to_string().contains("206"), "{error}");
}

#[test]
fn a_short_body_is_an_error() {
    let data = noise(LEN, 15);
    let server = Server::start(&[("file", &data)]);
    server.set_mode(Mode::CloseAfter(100));
    let mut file = HttpFile::new(
        reqwest::blocking::Client::new(),
        server.url("file"),
        0,
        LEN as u64,
        READAHEAD,
    );
    file.read(&mut [0u8; 16]).unwrap_err();
    server.set_mode(Mode::Normal);
    assert_eq!(read_at(&mut file, 0, 16), &data[..16]);
}
