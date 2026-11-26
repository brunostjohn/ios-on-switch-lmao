// SPDX-License-Identifier: GPL-3.0-or-later
use image4::der::Encode;
use image4::payload::ComprInfo;
use image4::{PayloadRef, Tag};
use sha2::{Digest, Sha256, Sha384};

use crate::{Error, device_tree, kernelcache, payload};

const ARM64: u32 = 0x0100_000c;
const X86_64: u32 = 0x0100_0007;

fn im4p(fourcc: &[u8; 4], data: &[u8], compr_info: Option<ComprInfo>) -> Vec<u8> {
    let mut payload = PayloadRef::new(Tag::from_bytes(*fourcc), "switch_img4 test", data).unwrap();
    payload.set_compr_info(compr_info);
    payload.to_der().unwrap()
}

fn lzfse(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    lzfse_rust::encode_bytes(data, &mut out).unwrap();
    out
}

fn lzfse_info(len: usize) -> Option<ComprInfo> {
    Some(ComprInfo {
        algo: 1,
        uncompressed_size: len as u32,
    })
}

fn sample() -> Vec<u8> {
    (0..50_000u32).flat_map(|i| (i % 251).to_le_bytes()).collect()
}

fn sha256(data: &[u8]) -> [u8; 32] {
    let mut digest = [0u8; 32];
    digest.copy_from_slice(&Sha256::digest(data));
    digest
}

// Each slice at its own page, as a universal binary lays them out.
fn fat(slices: &[(u32, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&0xcafe_babeu32.to_be_bytes());
    out.extend_from_slice(&(slices.len() as u32).to_be_bytes());
    for (i, (cpu, body)) in slices.iter().enumerate() {
        for word in [*cpu, 0, 4096 * (i as u32 + 1), body.len() as u32, 12] {
            out.extend_from_slice(&word.to_be_bytes());
        }
    }
    for (_, body) in slices {
        out.resize(out.len().next_multiple_of(4096), 0);
        out.extend_from_slice(body);
    }
    out
}

#[test]
fn an_lzfse_payload_decodes_to_its_compression_info_size() {
    let data = sample();
    let im4p = im4p(b"dtre", &lzfse(&data), lzfse_info(data.len()));
    assert!(payload(&im4p, *b"dtre", None).unwrap() == data);
    assert!(device_tree(&im4p).unwrap() == data);
}

#[test]
fn a_raw_payload_is_returned_as_it_is() {
    let data = b"\x02\0\0\0a trust cache".to_vec();
    let im4p = im4p(b"trst", &data, None);
    assert_eq!(payload(&im4p, *b"trst", None).unwrap(), data);
}

#[test]
fn the_manifest_digest_is_checked() {
    let im4p = im4p(b"trcs", b"\x02\0\0\0", None);
    let mut digest = [0u8; 48];
    digest.copy_from_slice(&Sha384::digest(&im4p));
    assert!(payload(&im4p, *b"trcs", Some(&digest)).is_ok());
    digest[0] ^= 1;
    assert!(matches!(
        payload(&im4p, *b"trcs", Some(&digest)),
        Err(Error::ManifestDigest)
    ));
}

#[test]
fn another_type_is_refused() {
    let im4p = im4p(b"trca", b"\x02\0\0\0", None);
    assert!(matches!(payload(&im4p, *b"trst", None), Err(Error::Fourcc { .. })));
    assert!(matches!(device_tree(&im4p), Err(Error::Fourcc { .. })));
}

#[test]
fn a_size_other_than_the_compression_info_is_refused() {
    let data = sample();
    let im4p = im4p(b"dtre", &lzfse(&data), lzfse_info(data.len() + 1));
    assert!(matches!(device_tree(&im4p), Err(Error::Size { .. })));
}

#[test]
fn another_compression_algorithm_is_refused() {
    let data = sample();
    let info = Some(ComprInfo {
        algo: 2,
        uncompressed_size: data.len() as u32,
    });
    assert!(matches!(
        device_tree(&im4p(b"dtre", &lzfse(&data), info)),
        Err(Error::Algorithm(2))
    ));
}

#[test]
fn a_device_tree_or_compressed_payload_that_is_not_lzfse_is_refused() {
    let data = sample();
    assert!(matches!(device_tree(&im4p(b"dtre", &data, None)), Err(Error::NotLzfse)));
    let im4p = im4p(b"dtre", &data, lzfse_info(data.len()));
    assert!(matches!(payload(&im4p, *b"dtre", None), Err(Error::NotLzfse)));
}

#[test]
fn a_fat_kernelcache_gives_its_one_arm64_slice() {
    let slice = b"\xcf\xfa\xed\xfe the arm64 kernel";
    let image = fat(&[(X86_64, b"\xcf\xfa\xed\xfe x86_64"), (ARM64, slice)]);
    let im4p = im4p(b"krnl", &lzfse(&image), lzfse_info(image.len()));
    assert_eq!(kernelcache(&im4p, &sha256(slice)).unwrap(), slice);
}

#[test]
fn a_fat_kernelcache_without_exactly_one_arm64_slice_is_refused() {
    for image in [fat(&[(ARM64, b"one"), (ARM64, b"two")]), fat(&[(X86_64, b"one")])] {
        let im4p = im4p(b"krnl", &lzfse(&image), lzfse_info(image.len()));
        assert!(matches!(kernelcache(&im4p, &[0; 32]), Err(Error::Fat(_))));
    }
}

#[test]
fn a_thin_kernelcache_is_kept_whole_and_checked_against_its_pin() {
    let image = sample();
    let im4p = im4p(b"krnl", &lzfse(&image), lzfse_info(image.len()));
    assert!(kernelcache(&im4p, &sha256(&image)).unwrap() == image);
    assert!(matches!(kernelcache(&im4p, &[0; 32]), Err(Error::Pin { .. })));
}
