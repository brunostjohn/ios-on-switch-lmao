// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::{Path, PathBuf};

use crate::align::{AlignedBuf, Piece, Split, align_down, align_up, split};
use crate::model::{Bus, Disk, Partition, Volume};
use crate::policy::{Refusal, is_linux_system_mount, is_macos_system_mount};

fn piece(offset: u64, data: std::ops::Range<usize>) -> Option<Piece> {
    Some(Piece { offset, data })
}

#[test]
fn split_aligned_access_is_all_body() {
    assert_eq!(
        split(4096, 8192, 512),
        Split {
            head: None,
            body: piece(4096, 0..8192),
            tail: None
        }
    );
}

#[test]
fn split_inside_one_sector_is_all_head() {
    assert_eq!(
        split(10, 10, 512),
        Split {
            head: piece(10, 0..10),
            body: None,
            tail: None
        }
    );
    assert_eq!(
        split(0, 100, 512),
        Split {
            head: None,
            body: None,
            tail: piece(0, 0..100)
        }
    );
}

#[test]
fn split_unaligned_both_ends() {
    assert_eq!(
        split(10, 1024, 512),
        Split {
            head: piece(10, 0..502),
            body: piece(512, 502..1014),
            tail: piece(1024, 1014..1024)
        }
    );
}

#[test]
fn split_across_one_boundary() {
    assert_eq!(
        split(500, 100, 512),
        Split {
            head: piece(500, 0..12),
            body: None,
            tail: piece(512, 12..100)
        }
    );
    assert_eq!(
        split(4000, 200, 4096),
        Split {
            head: piece(4000, 0..96),
            body: None,
            tail: piece(4096, 96..200)
        }
    );
}

#[test]
fn split_covers_every_byte_once() {
    for sector in [512u32, 4096] {
        for offset in [0u64, 1, 511, 512, 513, 4095, 4097] {
            for len in [0usize, 1, 511, 512, 513, 4096, 9000] {
                let s = split(offset, len, sector);
                let mut next = 0;
                for p in [s.head, s.body.clone(), s.tail].into_iter().flatten() {
                    assert_eq!(p.data.start, next);
                    assert_eq!(p.offset, offset + p.data.start as u64);
                    next = p.data.end;
                }
                assert_eq!(next, len, "{offset} {len} {sector}");
                if let Some(body) = s.body {
                    assert_eq!(body.offset % u64::from(sector), 0);
                    assert_eq!(body.data.len() % sector as usize, 0);
                }
            }
        }
    }
}

#[test]
fn rounding() {
    assert_eq!(align_down(1000, 512), 512);
    assert_eq!(align_up(1000, 512), 1024);
    assert_eq!(align_up(1024, 512), 1024);
}

#[test]
fn aligned_buffer() {
    for align in [512, 4096, 16384] {
        let b = AlignedBuf::new(8192, align);
        assert_eq!(b.as_ptr() as usize % align, 0);
        assert_eq!(b.len(), 8192);
        assert!(b.iter().all(|&x| x == 0));
    }
    assert_eq!(AlignedBuf::new(0, 4096).len(), 0);
}

fn card() -> Disk {
    Disk {
        id: "test:1".into(),
        path: PathBuf::from("/dev/disk4"),
        size: 64 << 30,
        logical_sector: 512,
        physical_sector: 512,
        removable: true,
        internal: false,
        system: false,
        read_only: false,
        model: "Reader".into(),
        bus: Bus::Usb,
        partitions: vec![Partition {
            path: PathBuf::from("/dev/disk4s1"),
            number: 1,
            offset: 1 << 20,
            size: 1 << 30,
            name: String::new(),
            kind: "0c".into(),
            volumes: vec![Volume {
                mount_point: PathBuf::from("/Volumes/SWITCH SD"),
                file_system: "msdos".into(),
            }],
        }],
        volumes: Vec::new(),
    }
}

#[test]
fn removable_card_is_pickable() {
    assert_eq!(card().refusal(), None);
    assert_eq!(card().mounted().count(), 1);
}

#[test]
fn built_in_reader_card_is_pickable() {
    let disk = Disk {
        internal: true,
        bus: Bus::Sd,
        ..card()
    };
    assert!(disk.pickable());
}

#[test]
fn internal_fixed_disk_is_refused() {
    let disk = Disk {
        internal: true,
        removable: false,
        bus: Bus::Nvme,
        ..card()
    };
    assert_eq!(disk.refusal(), Some(Refusal::Internal));
}

#[test]
fn system_disk_is_refused_even_when_removable() {
    let disk = Disk { system: true, ..card() };
    assert_eq!(disk.refusal(), Some(Refusal::System));
}

#[test]
fn locked_or_empty_reader_is_refused() {
    assert_eq!(
        Disk {
            read_only: true,
            ..card()
        }
        .refusal(),
        Some(Refusal::ReadOnly)
    );
    assert_eq!(Disk { size: 0, ..card() }.refusal(), Some(Refusal::NoMedia));
}

#[test]
fn system_mounts() {
    assert!(is_macos_system_mount(Path::new("/")));
    assert!(is_macos_system_mount(Path::new("/System/Volumes/Data")));
    assert!(!is_macos_system_mount(Path::new("/Volumes/SWITCH SD")));
    assert!(is_linux_system_mount(Path::new("/boot/efi")));
    assert!(is_linux_system_mount(Path::new("[SWAP]")));
    assert!(!is_linux_system_mount(Path::new("/media/me/SWITCH SD")));
}
