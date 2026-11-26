// SPDX-License-Identifier: GPL-3.0-or-later
//! The journal's on-card format and its reading after a cut, and the size picker's rules.

use std::io;

use switch_card::Device;
use switch_card_testing::SparseImage;
use switch_partition::journal::{self, FieldKind, Phase, Record, State, Step};
use switch_partition::{Fingerprint, FsKind, Limits, Plan, minimum_ios_bytes};
use switch_partition_testing::cards::{self, FAT_FIRST};

const GIB: u64 = 1 << 30;

fn plan() -> Plan {
    Plan {
        card_sectors: 1 << 30,
        fat_first: 32768,
        fat_sectors: 1 << 29,
        fat_new_sectors: 1 << 28,
        entry_first: 32768 + (1 << 28),
        entry_sectors: 64 << 21,
        entry_index: 10,
        entry_guid: [7; 16],
        fs: FsKind::Fat32,
        new_gpt: Some([9; 16]),
        fingerprint: Fingerprint {
            vol_serial: 0x1234_5678,
            vol_boot_sig: 0x29,
            vol_total: 1 << 29,
            boot_crc: 0xdead_beef,
            table_crc: 0xcafe_babe,
        },
    }
}

fn every_record() -> Step {
    Step {
        groups: vec![
            vec![
                Record::Chain {
                    first: 5,
                    len: 3,
                    last: 0x0fff_ffff,
                    loose: true,
                },
                Record::Bits {
                    first: 5,
                    len: 3,
                    set: true,
                },
                Record::Zero { lba: 999, count: 31 },
            ],
            vec![
                Record::Fat {
                    index: 4,
                    old: 100,
                    new: 5,
                },
                Record::Field {
                    lba: 1 << 33,
                    offset: 480,
                    kind: FieldKind::Fat32Cluster,
                    old: 100,
                    new: 5,
                },
                Record::Field {
                    lba: 3,
                    offset: 2,
                    kind: FieldKind::Le16,
                    old: 0xbeef,
                    new: 0xcafe,
                },
                Record::Field {
                    lba: 10,
                    offset: 32,
                    kind: FieldKind::ExfatChecksum,
                    old: 0x1111,
                    new: 0x2222,
                },
                Record::Field {
                    lba: 10,
                    offset: 64,
                    kind: FieldKind::ExfatStreamFlags,
                    old: 3,
                    new: 1,
                },
                Record::Field {
                    lba: 10,
                    offset: 64,
                    kind: FieldKind::ExfatStreamCluster,
                    old: 100,
                    new: 5,
                },
                Record::Field {
                    lba: 11,
                    offset: 32,
                    kind: FieldKind::ExfatBitmapLen,
                    old: 8192,
                    new: 4096,
                },
                Record::DotDot {
                    old: 100,
                    new: 5,
                    root: false,
                },
            ],
            vec![
                Record::Bits {
                    first: 100,
                    len: 3,
                    set: false,
                },
                Record::Free {
                    first: 100,
                    len: 3,
                    last: 0x0fff_fff8,
                },
                Record::Sector {
                    lba: 0,
                    old: 0x1234_5678,
                    new: Box::new([0xa5; 512]),
                },
            ],
        ],
    }
}

#[test]
fn a_step_reads_back_as_written() {
    let mut image = SparseImage::new(1 << 16);
    let step = every_record();
    journal::begin(&mut image, &plan()).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Finished(h) => assert_eq!((h.seq, h.phase, h.plan), (0, Phase::Relocate, plan())),
        other => panic!("{other:?}"),
    }
    journal::commit(&mut image, 1, Phase::Shrink, &plan(), &step).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Pending(h, s) => assert_eq!((h.seq, h.phase, h.plan, s), (1, Phase::Shrink, plan(), step)),
        other => panic!("{other:?}"),
    }
    journal::finish(&mut image, 2, Phase::Shrink, &plan()).unwrap();
    match journal::read(&mut image).unwrap() {
        State::Finished(h) => assert_eq!((h.seq, h.phase, h.plan), (2, Phase::Shrink, plan())),
        other => panic!("{other:?}"),
    }
}

/// The next step's body went down, its header didn't: the step in the header finished.
#[test]
fn a_body_the_header_does_not_match_is_a_finished_step() {
    let mut image = SparseImage::new(1 << 16);
    journal::begin(&mut image, &plan()).unwrap();
    journal::commit(&mut image, 1, Phase::Relocate, &plan(), &every_record()).unwrap();
    let mut body = image.read_bytes(journal::FIRST + 1, journal::SECTORS - 1);
    body[..8].copy_from_slice(&2u64.to_le_bytes());
    image.set(journal::FIRST + 1, &body);
    assert!(matches!(journal::read(&mut image).unwrap(), State::Finished(h) if h.seq == 1));
    journal::clear(&mut image).unwrap();
    assert_eq!(journal::read(&mut image).unwrap(), State::Empty);
}

#[test]
fn data_that_is_not_a_journal_is_refused() {
    let mut image = SparseImage::new(1 << 16);
    image.write_sectors(journal::FIRST + 9, &[1; 512]).unwrap();
    assert!(journal::read(&mut image).is_err());
    let mut image = SparseImage::new(1 << 16);
    journal::begin(&mut image, &plan()).unwrap();
    let mut header = image.sector(journal::FIRST);
    header[200] ^= 1;
    image.set(journal::FIRST, &header);
    assert!(journal::read(&mut image).is_err());
}

#[test]
fn the_picker_never_goes_under_16_gib_or_the_image_plus_4() {
    assert_eq!(minimum_ios_bytes(0), 16 * GIB);
    assert_eq!(minimum_ios_bytes(12 * GIB + GIB / 2), 16 * GIB + GIB / 2);
    assert_eq!(minimum_ios_bytes(20 * GIB + 1), 24 * GIB + (16 << 20));
    let roomy = Limits::new(5 * GIB, 300 * GIB);
    assert_eq!((roomy.min, roomy.default, roomy.max), (16 * GIB, 64 * GIB, 300 * GIB));
    let big_image = Limits::new(70 * GIB, 300 * GIB);
    assert_eq!(big_image.default, 74 * GIB);
    let small_card = Limits::new(5 * GIB, 40 * GIB + 5);
    assert_eq!((small_card.default, small_card.max), (40 * GIB, 40 * GIB));
    assert!(!Limits::new(5 * GIB, 10 * GIB).possible());
}

struct MaxTransferDevice {
    inner: SparseImage,
    max_bytes: usize,
    full_chunks: usize,
}

impl Device for MaxTransferDevice {
    fn sector_size(&self) -> u32 {
        self.inner.sector_size()
    }

    fn sector_count(&self) -> u64 {
        self.inner.sector_count()
    }

    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> io::Result<()> {
        const MAX_BYTES: usize = 16_384 * 512;
        assert!(buf.len() <= MAX_BYTES, "read exceeded MAX_REQUEST: {} bytes", buf.len());
        self.max_bytes = self.max_bytes.max(buf.len());
        if buf.len() == MAX_BYTES {
            self.full_chunks += 1;
        }
        self.inner.read_sectors(lba, buf)
    }

    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> io::Result<()> {
        const MAX_BYTES: usize = 16_384 * 512;
        assert!(
            buf.len() <= MAX_BYTES,
            "write exceeded MAX_REQUEST: {} bytes",
            buf.len()
        );
        self.inner.write_sectors(lba, buf)
    }

    fn barrier(&mut self) -> io::Result<()> {
        self.inner.barrier()
    }
}

#[test]
fn exfat_read_clusters_chunks_16mib_clusters_to_max_request() {
    let spc_shift = 15u8;
    let spc = 1u64 << spc_shift;
    let fat_off = 24u32;
    let fat_len = 8u32;
    let heap_off = 32u32;
    let cluster_count = 4u32;
    let vol_len = u64::from(heap_off) + u64::from(cluster_count) * spc;
    let card_sectors = FAT_FIRST + vol_len + 64;
    let mut img = cards::gpt_exfat(card_sectors, vol_len);

    let mut region = vec![0u8; 12 * 512];
    region[..3].copy_from_slice(&[0xeb, 0x76, 0x90]);
    region[3..11].copy_from_slice(b"EXFAT   ");
    region[64..72].copy_from_slice(&FAT_FIRST.to_le_bytes());
    region[72..80].copy_from_slice(&vol_len.to_le_bytes());
    region[80..84].copy_from_slice(&fat_off.to_le_bytes());
    region[84..88].copy_from_slice(&fat_len.to_le_bytes());
    region[88..92].copy_from_slice(&heap_off.to_le_bytes());
    region[92..96].copy_from_slice(&cluster_count.to_le_bytes());
    region[96..100].copy_from_slice(&4u32.to_le_bytes());
    region[100..104].copy_from_slice(&0x1234_5678u32.to_le_bytes());
    region[104..106].copy_from_slice(&0x0100u16.to_le_bytes());
    region[108] = 9;
    region[109] = spc_shift;
    region[110] = 1;
    region[111] = 0x80;
    region[112] = 0xff;
    region[510] = 0x55;
    region[511] = 0xaa;
    for s in 1..9usize {
        region[s * 512 + 508..(s + 1) * 512].copy_from_slice(&[0, 0, 0x55, 0xaa]);
    }
    let mut sum = 0u32;
    for (i, &b) in region[..11 * 512].iter().enumerate() {
        if i == 106 || i == 107 || i == 112 {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    let sum_bytes = sum.to_le_bytes();
    for slot in region[11 * 512..12 * 512].as_chunks_mut::<4>().0 {
        slot.copy_from_slice(&sum_bytes);
    }
    img.set(FAT_FIRST, &region);
    img.set(FAT_FIRST + 12, &region);

    let mut fat0 = [0u8; 512];
    for (i, v) in [0xffff_fff8u32, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff]
        .iter()
        .enumerate()
    {
        fat0[i * 4..(i + 1) * 4].copy_from_slice(&v.to_le_bytes());
    }
    img.set(FAT_FIRST + u64::from(fat_off), &fat0);

    let cluster_lba = |c: u32| FAT_FIRST + u64::from(heap_off) + u64::from(c - 2) * spc;
    let mut bm = [0u8; 512];
    bm[0] = 0b0000_0111;
    img.set(cluster_lba(2), &bm);

    let mut root = [0u8; 512];
    root[0] = 0x81;
    root[20..24].copy_from_slice(&2u32.to_le_bytes());
    root[24..32].copy_from_slice(&1u64.to_le_bytes());
    root[32] = 0x82;
    root[32 + 20..32 + 24].copy_from_slice(&3u32.to_le_bytes());
    root[32 + 24..32 + 32].copy_from_slice(&2u64.to_le_bytes());
    img.set(cluster_lba(4), &root);

    let mut dev = MaxTransferDevice {
        inner: img,
        max_bytes: 0,
        full_chunks: 0,
    };
    switch_partition::survey(&mut dev).unwrap();
    assert_eq!(dev.max_bytes, 16_384 * 512);
    assert!(dev.full_chunks >= 6);
}
