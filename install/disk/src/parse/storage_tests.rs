// SPDX-License-Identifier: GPL-3.0-or-later
use super::storage::*;
use crate::model::Bus;

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

#[test]
fn device_descriptor_of_a_usb_reader() {
    let mut b = vec![0u8; 80];
    b[DEVICE_DESCRIPTOR_REMOVABLE] = 1;
    put32(&mut b, DEVICE_DESCRIPTOR_VENDOR, 40);
    put32(&mut b, DEVICE_DESCRIPTOR_PRODUCT, 50);
    put32(&mut b, DEVICE_DESCRIPTOR_BUS, 7);
    b[40..48].copy_from_slice(b"Generic ");
    b[50..62].copy_from_slice(b"SD/MMC Card ");
    let d = device_descriptor(&b).unwrap();
    assert!(d.removable);
    assert_eq!(d.bus, Bus::Usb);
    assert_eq!(d.model, "Generic SD/MMC Card");
}

#[test]
fn device_descriptor_without_strings() {
    let mut b = vec![0u8; 40];
    put32(&mut b, DEVICE_DESCRIPTOR_BUS, 0x11);
    let d = device_descriptor(&b).unwrap();
    assert!(!d.removable);
    assert_eq!(d.bus, Bus::Nvme);
    assert_eq!(d.model, "");
    assert!(device_descriptor(&b[..20]).is_none());
}

#[test]
fn alignment_descriptor() {
    let mut b = vec![0u8; 28];
    put32(&mut b, ALIGNMENT_LOGICAL, 512);
    put32(&mut b, ALIGNMENT_PHYSICAL, 4096);
    assert_eq!(alignment(&b), Some((512, 4096)));
}

#[test]
fn gpt_layout() {
    let mut b = vec![0u8; LAYOUT_HEADER + 2 * LAYOUT_ENTRY];
    put32(&mut b, 0, 1);
    put32(&mut b, 4, 2);
    for (i, (number, offset, name)) in [(1u32, 1u64 << 20, "hos_data"), (2, 1 << 30, "iOS")]
        .into_iter()
        .enumerate()
    {
        let e = LAYOUT_HEADER + i * LAYOUT_ENTRY;
        put32(&mut b, e, 1);
        put64(&mut b, e + 8, offset);
        put64(&mut b, e + 16, 16 << 20);
        put32(&mut b, e + 24, number);
        let apfs = [
            0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
        ];
        b[e + 32..e + 48].copy_from_slice(&apfs);
        for (j, u) in name.encode_utf16().enumerate() {
            b[e + 72 + 2 * j..e + 74 + 2 * j].copy_from_slice(&u.to_le_bytes());
        }
    }
    let entries = drive_layout(&b).unwrap();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[1].number, 2);
    assert_eq!(entries[1].offset, 1 << 30);
    assert_eq!(entries[1].size, 16 << 20);
    assert_eq!(entries[1].name, "iOS");
    assert_eq!(entries[0].kind, "7C3457EF-0000-11AA-AA11-00306543ECAC");
}

#[test]
fn mbr_layout_skips_empty_slots() {
    let mut b = vec![0u8; LAYOUT_HEADER + 4 * LAYOUT_ENTRY];
    put32(&mut b, 4, 4);
    let e = LAYOUT_HEADER;
    put64(&mut b, e + 8, 1 << 20);
    put64(&mut b, e + 16, 1 << 30);
    put32(&mut b, e + 24, 1);
    b[e + 32] = 0x0c;
    let entries = drive_layout(&b).unwrap();
    assert_eq!(entries.len(), 1);
    assert_eq!(entries[0].kind, "0c");
    assert!(drive_layout(&b[..LAYOUT_HEADER + LAYOUT_ENTRY]).is_none());
}

#[test]
fn extents() {
    let mut b = vec![0u8; EXTENTS_HEADER + 2 * EXTENT];
    put32(&mut b, 0, 2);
    put32(&mut b, EXTENTS_HEADER, 3);
    put64(&mut b, EXTENTS_HEADER + 8, 1 << 20);
    put64(&mut b, EXTENTS_HEADER + 16, 4096);
    put32(&mut b, EXTENTS_HEADER + EXTENT, 4);
    let e = disk_extents(&b).unwrap();
    assert_eq!(
        e[0],
        Extent {
            disk: 3,
            offset: 1 << 20,
            length: 4096
        }
    );
    assert_eq!(e[1].disk, 4);
}

#[test]
fn disk_geometry() {
    let mut b = vec![0u8; 40];
    put32(&mut b, GEOMETRY_SECTOR, 512);
    put64(&mut b, GEOMETRY_SIZE, 64 << 30);
    assert_eq!(geometry(&b), Some((64 << 30, 512)));
    assert!(geometry(&b[..30]).is_none());
}
