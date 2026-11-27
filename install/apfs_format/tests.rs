// SPDX-License-Identifier: GPL-3.0-or-later
use crate::object::{BLOCK_SIZE, checksum_ok, fletcher64, header, seal};

/// A block from its non-zero 16-byte rows.
pub fn block_from_rows(rows: &[(usize, [u8; 16])]) -> Vec<u8> {
    let mut block = vec![0u8; BLOCK_SIZE];
    for (at, row) in rows {
        block[*at..*at + 16].copy_from_slice(row);
    }
    block
}

fn apple_24b119_reaper() -> Vec<u8> {
    block_from_rows(&[
        (
            0x00,
            [
                0x07, 0x6d, 0xd8, 0xb8, 0x65, 0xc3, 0x25, 0x87, 0x01, 0x04, 0, 0, 0, 0, 0, 0,
            ],
        ),
        (0x10, [0xd3, 0x01, 0, 0, 0, 0, 0, 0, 0x11, 0, 0, 0x80, 0, 0, 0, 0]),
        (0x20, [0x02, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0]),
        (0x30, [0x09, 0xd5, 0, 0, 0, 0, 0, 0, 0x09, 0xd5, 0, 0, 0, 0, 0, 0]),
        (0x40, [0x01, 0, 0, 0, 0x01, 0, 0, 0, 0x02, 0, 0, 0x40, 0x00, 0x10, 0, 0]),
        (0x60, [0x01, 0, 0, 0, 0, 0, 0, 0, 0x03, 0, 0, 0, 0x90, 0x0f, 0, 0]),
    ])
}

#[test]
fn fletcher_matches_apples_reaper() {
    let block = apple_24b119_reaper();
    assert!(checksum_ok(&block));
    assert_eq!(fletcher64(&block), 0x8725_c365_b8d8_6d07);
}

#[test]
fn seal_then_check() {
    let mut block = apple_24b119_reaper();
    block[0x30] = 0x0a;
    assert!(!checksum_ok(&block));
    seal(&mut block);
    assert!(checksum_ok(&block));
}

#[test]
fn header_fields() {
    let h = header(&apple_24b119_reaper());
    assert_eq!(h.oid.get(), 1025);
    assert_eq!(h.xid.get(), 467);
    assert_eq!(h.object_type(), crate::object::TYPE_NX_REAPER);
    assert_eq!(h.storage(), crate::object::OBJ_EPHEMERAL);
}
