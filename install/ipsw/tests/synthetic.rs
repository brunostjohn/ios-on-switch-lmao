// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{File, OpenOptions};
use std::io::{self, Cursor, Seek, SeekFrom, Write};

use flate2::Compression;
use flate2::write::DeflateEncoder;
use sha2::{Digest, Sha256};
use switch_ipsw::{Container, Error, Member, extract, os_container, sniff, udif_disk};
use udif::{CompressionMethod, DmgWriter};

/// Compressible but not trivially so, and longer than the extractor's chunks.
fn content(len: usize) -> Vec<u8> {
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    (0..len)
        .map(|i| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            if i % 3 == 0 { (x >> 56) as u8 } else { (i / 4096) as u8 }
        })
        .collect()
}

fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

fn deflate(data: &[u8]) -> Vec<u8> {
    let mut encoder = DeflateEncoder::new(Vec::new(), Compression::default());
    encoder.write_all(data).unwrap();
    encoder.finish().unwrap()
}

fn member(stored: &[u8], data: &[u8], deflated: bool) -> Member {
    Member {
        member: "Firmware/test.im4p".into(),
        offset: 0,
        stored_size: stored.len() as u64,
        size: data.len() as u64,
        sha256: sha256(stored),
        content_sha256: sha256(data),
        deflated,
        aea_key: None,
    }
}

#[test]
fn inflates_a_member() {
    let data = content((9 << 20) + 123);
    let stored = deflate(&data);
    let mut out = Vec::new();
    let written = extract(&stored[..], &member(&stored, &data, true), &mut out).unwrap();
    assert_eq!(out, data);
    assert_eq!((written.len, written.sha256), (data.len() as u64, sha256(&data)));
}

#[test]
fn copies_a_stored_member() {
    let data = content(5 << 20);
    let mut out = Vec::new();
    extract(&data[..], &member(&data, &data, false), &mut out).unwrap();
    assert_eq!(out, data);
}

#[test]
fn a_truncated_stream_is_refused() {
    let data = content(1 << 20);
    let stored = deflate(&data);
    let cut = &stored[..stored.len() - 7];
    let error = extract(cut, &member(cut, &data, true), &mut io::sink()).unwrap_err();
    assert!(matches!(error, Error::Deflate { .. }), "{error}");
}

#[test]
fn bytes_after_the_stream_are_refused() {
    let data = content(1 << 20);
    let mut stored = deflate(&data);
    stored.extend_from_slice(b"zip");
    let error = extract(&stored[..], &member(&stored, &data, true), &mut io::sink()).unwrap_err();
    assert!(matches!(error, Error::Deflate { .. }), "{error}");
}

#[test]
fn a_short_source_is_refused() {
    let data = content(1 << 20);
    let mut pin = member(&data, &data, false);
    pin.stored_size += 5;
    let error = extract(&data[..], &pin, &mut io::sink()).unwrap_err();
    assert!(matches!(error, Error::Stored { .. }), "{error}");
}

#[test]
fn other_content_is_refused() {
    let data = content(1 << 20);
    let stored = deflate(&data);
    let mut pin = member(&stored, &data, true);
    pin.content_sha256[5] ^= 1;
    let error = extract(&stored[..], &pin, &mut io::sink()).unwrap_err();
    assert!(matches!(error, Error::Content { .. }), "{error}");
}

#[test]
fn an_encrypted_member_is_refused() {
    let mut pin = member(b"", b"", false);
    pin.aea_key = Some("base64:".into());
    assert!(matches!(
        extract(&b""[..], &pin, &mut io::sink()),
        Err(Error::Encrypted(_))
    ));
}

struct FailAfter(usize);

impl Write for FailAfter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        if data.len() > self.0 {
            return Err(io::Error::other("disk full"));
        }
        self.0 -= data.len();
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[test]
fn a_failed_write_is_the_error() {
    let data = content(12 << 20);
    let stored = deflate(&data);
    let error = extract(&stored[..], &member(&stored, &data, true), &mut FailAfter(5 << 20)).unwrap_err();
    assert!(
        matches!(&error, Error::Write(e) if e.to_string() == "disk full"),
        "{error}"
    );
}

/// A UDIF image like 21H461's: a small partition, then an APFS one in LZFSE chunks.
fn udif(dir: &tempfile::TempDir, head: &[u8], apfs: &[u8]) -> File {
    let path = dir.path().join("os.dmg");
    let mut writer = DmgWriter::create(&path)
        .unwrap()
        .compression(CompressionMethod::Lzfse)
        .chunk_size(1 << 20);
    writer
        .add_partition("Protective Master Boot Record (MBR : 0)", head)
        .unwrap();
    writer.add_partition("Apple (Apple_APFS : 4)", apfs).unwrap();
    writer.finish().unwrap();
    let mut image = OpenOptions::new().read(true).write(true).open(path).unwrap();
    // udif's writer zero-pads its trailer past byte 444, leaving SectorCount (0x1ec) zero.
    set_sectors(&mut image, (head.len().div_ceil(512) + apfs.len().div_ceil(512)) as u64);
    image
}

fn set_sectors(image: &mut File, sectors: u64) {
    image.seek(SeekFrom::End(0x1ec - 512)).unwrap();
    image.write_all(&sectors.to_be_bytes()).unwrap();
    image.seek(SeekFrom::Start(0)).unwrap();
}

fn apfs_image(len: usize) -> Vec<u8> {
    let mut image = content(len);
    image[0x20..0x24].copy_from_slice(b"NXSB");
    image
}

#[test]
fn unwraps_a_udif_image() {
    let dir = tempfile::tempdir().unwrap();
    let head = content(1000);
    let apfs = apfs_image(3 << 20);
    let mut disk = Vec::new();
    udif_disk(udif(&dir, &head, &apfs), &mut disk).unwrap();
    let mut expected = head.clone();
    expected.resize(1024, 0);
    expected.extend_from_slice(&apfs);
    assert_eq!(disk, expected);
    let mut container = Vec::new();
    let written = os_container(udif(&dir, &head, &apfs), &mut container, Container::Udif).unwrap();
    assert_eq!(container, apfs);
    assert_eq!(written.sha256, sha256(&apfs));
}

#[test]
fn copies_a_bare_container() {
    let apfs = apfs_image(2 << 20);
    let mut container = Vec::new();
    os_container(Cursor::new(&apfs), &mut container, Container::Apfs).unwrap();
    assert_eq!(container, apfs);
}

#[test]
fn a_format_the_catalog_did_not_declare_is_refused() {
    let dir = tempfile::tempdir().unwrap();
    let apfs = apfs_image(1 << 20);
    let error = os_container(udif(&dir, &[0; 512], &apfs), &mut io::sink(), Container::Apfs).unwrap_err();
    assert!(
        matches!(
            error,
            Error::Container {
                found: Some(Container::Udif),
                ..
            }
        ),
        "{error}"
    );
    let error = os_container(Cursor::new(&apfs), &mut io::sink(), Container::Udif).unwrap_err();
    assert!(
        matches!(
            error,
            Error::Container {
                found: Some(Container::Apfs),
                ..
            }
        ),
        "{error}"
    );
}

#[test]
fn sniffs_both_formats() {
    let dir = tempfile::tempdir().unwrap();
    let apfs = apfs_image(1 << 20);
    assert_eq!(sniff(&mut Cursor::new(&apfs)).unwrap(), Some(Container::Apfs));
    assert_eq!(sniff(&mut udif(&dir, &[0; 512], &apfs)).unwrap(), Some(Container::Udif));
    assert_eq!(sniff(&mut Cursor::new(vec![0u8; 16])).unwrap(), None);
}

#[test]
fn the_trailer_sets_the_disk_length() {
    let dir = tempfile::tempdir().unwrap();
    let (head, apfs) = (content(512), apfs_image(1 << 20));
    let sectors = 1 + (1 << 20) / 512;
    let mut image = udif(&dir, &head, &apfs);
    set_sectors(&mut image, sectors + 3);
    let mut disk = Vec::new();
    udif_disk(&mut image, &mut disk).unwrap();
    assert_eq!(disk.len() as u64, (sectors + 3) * 512);
    assert!(disk[disk.len() - 3 * 512..].iter().all(|&b| b == 0));
    set_sectors(&mut image, sectors - 1);
    let error = udif_disk(&mut image, &mut io::sink()).unwrap_err();
    assert!(matches!(error, Error::Layout(_)), "{error}");
}
