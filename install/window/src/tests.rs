// SPDX-License-Identifier: GPL-3.0-or-later

use crate::constants::{Constants, NAMES, STAGED, STORED};
use crate::layout::kernel_phys_base;
use crate::{Error, lz4, pack};

fn numbered() -> Constants {
    let known: Vec<(&str, u64)> = NAMES
        .iter()
        .enumerate()
        .filter(|(_, n)| !STAGED.contains(n))
        .map(|(i, n)| (*n, i as u64))
        .collect();
    let mut constants = Constants::new(&known);
    constants.set("PACKED_BYTES", 3);
    constants.set("STREAM_BYTES", 4);
    constants.set("STAGE_ID", u64::MAX);
    constants
}

#[test]
fn constants_json_is_pythons_indent_1() {
    let mut constants = numbered();
    let empty = constants.json();
    assert!(empty.starts_with("{\n \"SOURCE\": 0,\n \"DEST\": 1,\n \"BYTES\": 2,\n \"PACKED_BYTES\": 3,"));
    assert!(empty.ends_with(": 29,\n \"FAST_COPY\": 30,\n \"BLOCKS\": []\n}\n"));
    assert!(empty.contains("\n \"STAGE_ID\": 18446744073709551615,\n"));
    constants.blocks = vec![[0, 4096, 0, 12 | STORED], [8192, 1 << 20, 12, 99]];
    assert!(constants.json().ends_with(
        "\": 30,\n \"BLOCKS\": [\n  [\n   0,\n   4096,\n   0,\n   9223372036854775820\n  ],\n  [\n   8192,\n   \
         1048576,\n   12,\n   99\n  ]\n ]\n}\n"
    ));
}

#[test]
fn constants_refuse_another_window_by_key() {
    let constants = numbered();
    assert!(constants.check_against(constants.json().as_bytes()).is_ok());
    let other = constants
        .json()
        .replace("\"CRC\": 12", "\"CRC\": 1")
        .replace("\"BLOCKS\": []", "\"BLOCKS\": [[0]]");
    match constants.check_against(other.as_bytes()) {
        Err(Error::LoaderBuiltForAnotherWindow(keys)) => assert_eq!(keys, ["BLOCKS", "CRC"]),
        other => panic!("{other:?}"),
    }
    assert!(matches!(constants.check_against(b"[]"), Err(Error::Constants(_))));
}

#[test]
fn extents_keep_short_zero_gaps_and_blocks_are_1_mib_from_each_start() {
    let mut image = vec![0u8; 4096 * 40];
    for page in [1, 2, 5, 25, 39] {
        image[page * 4096 + 7] = 1;
    }
    assert_eq!(pack::extents(&image), [(4096, 20480), (102400, 61440)]);
    assert_eq!(pack::extents(&[0; 8192]), []);
    assert_eq!(
        pack::blocks(&[(0, (2 << 20) + 4096)]),
        [(0, 1 << 20), (1 << 20, 1 << 20), (2 << 20, 4096)]
    );
}

#[test]
fn the_kernel_sits_above_the_bundle_at_its_offset_modulo_2_mib() {
    let base = 0xFFFF_FFF0_0307_C000;
    assert_eq!(kernel_phys_base(0xB200_0000, base, 0x1234), 0xB207_C000);
    assert_eq!(kernel_phys_base(0xB200_0000, base, 0x7_C001), 0xB227_C000);
}

#[test]
fn frames_the_loader_cannot_decode_are_refused() {
    let block = vec![7u8; 8192];
    let ours = lz4::compress_blocks(&[&block]).unwrap();
    assert!(!ours[0].stored && ours[0].body.len() < 100);
    let mut frame = vec![0x04, 0x22, 0x4D, 0x18, 0x60, 0x60, 0x51];
    frame.extend_from_slice(&(ours[0].body.len() as u32).to_le_bytes());
    frame.extend_from_slice(&ours[0].body);
    frame.extend_from_slice(&[0; 4]);
    let (decoded, parsed) = lz4::frame_blocks(&frame, block.len()).unwrap().pop().unwrap();
    assert_eq!((decoded, parsed), (block.len(), ours[0].clone()));
    assert!(lz4::frame_blocks(&frame, 0).is_err());
    let mut checksummed = frame.clone();
    checksummed[4] |= 0x10;
    assert!(lz4::frame_blocks(&checksummed, block.len()).is_err());
    assert!(lz4::frame_blocks(&frame[..frame.len() - 1], block.len()).is_err());
}
