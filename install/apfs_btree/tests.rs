// SPDX-License-Identifier: GPL-3.0-or-later
//! Nodes macOS 27.2's kext and newfs_apfs wrote, rebuilt byte for byte.
use super::*;

fn block_from_rows(rows: &[(usize, [u8; 16])]) -> Vec<u8> {
    let mut block = vec![0u8; BLOCK_SIZE];
    for (at, row) in rows {
        block[*at..*at + 16].copy_from_slice(row);
    }
    block
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

#[rustfmt::skip]
const EXTENTREF_ROWS: &[(usize, [u8; 16])] = &[
    (0x000, [0x27, 0x2b, 0xf2, 0x36, 0xea, 0xbe, 0x6b, 0xb9, 0xca, 0, 0, 0, 0, 0, 0, 0]),
    (0x010, [0x03, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0x40, 0x0f, 0, 0, 0]),
    (0x020, [0x03, 0, 0, 0, 0x04, 0, 0, 0, 0, 0, 0x40, 0, 0x20, 0, 0xf0, 0x0e]),
    (0x030, [0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0x08, 0, 0x14, 0, 0x14, 0]),
    (0x040, [0x08, 0, 0x08, 0, 0x28, 0, 0x14, 0, 0x10, 0, 0x08, 0, 0x3c, 0, 0x14, 0]),
    (0x050, [0x18, 0, 0x08, 0, 0x50, 0, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    (0x070, [0, 0, 0, 0, 0, 0, 0, 0, 0xc9, 0, 0, 0, 0, 0, 0, 0x20]),
    (0x080, [0xcb, 0, 0, 0, 0, 0, 0, 0x20, 0xcc, 0, 0, 0, 0, 0, 0, 0x20]),
    (0x090, [0xcd, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0, 0, 0, 0, 0]),
    (0xf80, [0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0x10]),
    (0xf90, [0x14, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0]),
    (0xfa0, [0, 0, 0, 0x10, 0x13, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0]),
    (0xfb0, [0x01, 0, 0, 0, 0, 0, 0, 0x10, 0x12, 0, 0, 0, 0, 0, 0, 0]),
    (0xfc0, [0x01, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0x10, 0x11, 0, 0, 0]),
    (0xfd0, [0, 0, 0, 0, 0x01, 0, 0, 0, 0x52, 0, 0, 0, 0, 0x10, 0, 0]),
    (0xfe0, [0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0, 0, 0, 0x14, 0, 0, 0]),
    (0xff0, [0x04, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0]),
];

#[test]
fn extent_reference_leaf() {
    let records: Vec<Record> = [(0xc9, 0x11), (0xcb, 0x12), (0xcc, 0x13), (0xcd, 0x14)]
        .iter()
        .map(|&(paddr, owner): &(u64, u64)| {
            let key = (paddr | 2 << 60).to_le_bytes().to_vec();
            let mut value = (1u64 | 1 << 60).to_le_bytes().to_vec();
            value.extend_from_slice(&owner.to_le_bytes());
            value.extend_from_slice(&1i32.to_le_bytes());
            (key, Some(value))
        })
        .collect();
    let built = leaf_root(&TreeShape::EXTENT_REF, 202, 3, &records).unwrap();
    assert_eq!(&built[..], &block_from_rows(EXTENTREF_ROWS)[..]);
}

#[test]
fn empty_snapshot_metadata_root() {
    #[rustfmt::skip]
    let apple = block_from_rows(&[
        (0x000, [0x6c, 0xf8, 0x12, 0x54, 0x67, 0xf6, 0x4a, 0x5c, 0xc4, 0, 0, 0, 0, 0, 0, 0]),
        (0x010, [0x02, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0x40, 0x10, 0, 0, 0]),
        (0x020, [0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0x60, 0x0f]),
        (0x030, [0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        (0xfd0, [0, 0, 0, 0, 0, 0, 0, 0, 0x52, 0, 0, 0, 0, 0x10, 0, 0]),
        (0xff0, [0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0]),
    ]);
    assert_eq!(&leaf_root(&TreeShape::SNAP_META, 196, 2, &[]).unwrap()[..], &apple[..]);
}

/// newfs_apfs's internal-pool free queue at 16 GiB: one ghost entry.
#[test]
fn free_queue_root_with_a_ghost() {
    #[rustfmt::skip]
    let apple = block_from_rows(&[
        (0x000, [0xc7, 0xb9, 0xcf, 0xd4, 0x49, 0x09, 0x9f, 0x9b, 0x03, 0x04, 0, 0, 0, 0, 0, 0]),
        (0x010, [0x02, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0x80, 0x09, 0, 0, 0]),
        (0x020, [0x07, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0x40, 0x02, 0x10, 0, 0x50, 0x0d]),
        (0x030, [0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0]),
        (0x270, [0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0]),
        (0x280, [0x8d, 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        (0xfd0, [0, 0, 0, 0, 0, 0, 0, 0, 0x0e, 0, 0, 0, 0, 0x10, 0, 0]),
        (0xfe0, [0x10, 0, 0, 0, 0x08, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0]),
        (0xff0, [0x01, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0]),
    ]);
    let key = hex("02000000000000008d28000000000000");
    let built = leaf_root(&TreeShape::FREE_QUEUE, 1027, 2, &[(key, None)]).unwrap();
    assert_eq!(&built[..], &apple[..]);
}

#[test]
fn omap_table_matches_apples() {
    let built = leaf_root(&TreeShape::OMAP, 208, 3, &[(vec![0; 16], Some(vec![0; 16]))]).unwrap();
    assert_eq!(&built[0x28..0x2c], &[0, 0, 0xc0, 0x01]);
}

#[test]
fn refuses_what_does_not_fit() {
    let big: Vec<Record> = (0..300u64)
        .map(|i| (i.to_le_bytes().to_vec(), Some(vec![0; 20])))
        .collect();
    assert!(matches!(
        leaf_root(&TreeShape::EXTENT_REF, 1, 1, &big),
        Err(BuildError::Overflow { .. })
    ));
    assert_eq!(
        leaf_root(&TreeShape::OMAP, 1, 1, &[(vec![0; 16], None)]),
        Err(BuildError::Ghost(0))
    );
    assert_eq!(
        leaf_root(&TreeShape::OMAP, 1, 1, &[(vec![0; 8], Some(vec![0; 16]))]),
        Err(BuildError::WrongSize(0))
    );
}
