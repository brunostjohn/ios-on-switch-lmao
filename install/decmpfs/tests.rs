// SPDX-License-Identifier: GPL-3.0-or-later
//! Hand-built files of each layout. The real check is the mount comparison (mount_test.sh): Apple's kernel reading
//! Apple's own compressed files.

use std::io::Write;

use flate2::Compression;
use flate2::write::ZlibEncoder;

use crate::{Error, Header, decompress};

const BLOCK: usize = 0x10000;

fn header(kind: u32, size: usize) -> Vec<u8> {
    let mut out = b"fpmc".to_vec();
    out.extend_from_slice(&kind.to_le_bytes());
    out.extend_from_slice(&(size as u64).to_le_bytes());
    out
}

fn zlib(data: &[u8]) -> Vec<u8> {
    let mut e = ZlibEncoder::new(Vec::new(), Compression::default());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn sample(len: usize) -> Vec<u8> {
    (0..len).map(|i| (i * 7 % 251) as u8).collect()
}

/// The resource fork of a type 4 file: a 256-byte header naming the data at 0x100, then the data's length, the block
/// count and (offset, length) pairs from the count, all but the first field little-endian.
fn zlib_fork(blocks: &[Vec<u8>]) -> Vec<u8> {
    let table = 4 + 8 * blocks.len();
    let mut data = (blocks.len() as u32).to_le_bytes().to_vec();
    let mut at = table;
    for b in blocks {
        data.extend_from_slice(&(at as u32).to_le_bytes());
        data.extend_from_slice(&(b.len() as u32).to_le_bytes());
        at += b.len();
    }
    for b in blocks {
        data.extend_from_slice(b);
    }
    let mut fork = vec![0u8; 0x100];
    fork[..4].copy_from_slice(&0x100u32.to_be_bytes());
    fork.extend_from_slice(&(data.len() as u32).to_be_bytes());
    fork.extend_from_slice(&data);
    fork
}

/// The resource fork of a type 14 file: n + 1 offsets, then the blocks.
fn offset_fork(blocks: &[Vec<u8>]) -> Vec<u8> {
    let mut at = 4 * (blocks.len() + 1);
    let mut fork = Vec::new();
    for b in blocks {
        fork.extend_from_slice(&(at as u32).to_le_bytes());
        at += b.len();
    }
    fork.extend_from_slice(&(at as u32).to_le_bytes());
    for b in blocks {
        fork.extend_from_slice(b);
    }
    fork
}

fn stored(data: &[u8]) -> Vec<u8> {
    let mut b = vec![0xff];
    b.extend_from_slice(data);
    b
}

#[test]
fn header_fields() {
    let h = Header::parse(&header(14, 70000)).unwrap();
    assert_eq!(h, Header { kind: 14, size: 70000 });
    assert!(h.in_resource_fork());
    assert!(!Header::parse(&header(9, 1)).unwrap().in_resource_fork());
    assert!(matches!(Header::parse(b"fpmc"), Err(Error::Header(_))));
    assert!(matches!(Header::parse(&[0u8; 16]), Err(Error::Header(_))));
}

#[test]
fn stored_in_the_xattr() {
    let mut x = header(9, 5);
    x.extend_from_slice(b"\xcchello");
    assert_eq!(decompress(&x, None).unwrap(), b"hello");
}

#[test]
fn zlib_in_the_xattr() {
    let data = sample(3000);
    let mut x = header(3, data.len());
    x.extend_from_slice(&zlib(&data));
    assert_eq!(decompress(&x, None).unwrap(), data);
}

#[test]
fn zlib_in_the_resource_fork() {
    let data = sample(BLOCK + 1234);
    let blocks = [zlib(&data[..BLOCK]), stored(&data[BLOCK..])];
    let fork = zlib_fork(&blocks);
    assert_eq!(decompress(&header(4, data.len()), Some(&fork)).unwrap(), data);
}

#[test]
fn lzbitmap_stored_blocks() {
    let data = sample(BLOCK + 10);
    let fork = offset_fork(&[stored(&data[..BLOCK]), stored(&data[BLOCK..])]);
    assert_eq!(decompress(&header(14, data.len()), Some(&fork)).unwrap(), data);
    let mut x = header(13, 3);
    x.extend_from_slice(&stored(b"abc"));
    assert_eq!(decompress(&x, None).unwrap(), b"abc");
}

#[test]
fn refusals() {
    assert!(matches!(
        decompress(&header(14, 3), None),
        Err(Error::NoResourceFork(14))
    ));
    assert!(matches!(decompress(&header(4, 3), None), Err(Error::NoResourceFork(4))));
    let fork = offset_fork(&[stored(b"abc")]);
    assert!(decompress(&header(14, 5), Some(&fork)).is_err());
    let mut short = header(9, 6);
    short.extend_from_slice(b"\xcchello");
    assert!(decompress(&short, None).is_err());
    let mut unknown = header(77, 1);
    unknown.push(0);
    assert!(decompress(&unknown, None).is_err());
}
