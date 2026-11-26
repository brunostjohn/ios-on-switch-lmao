// SPDX-License-Identifier: GPL-3.0-or-later
//! The boot entry on FAT32 and exFAT volumes: the allowlist, the replace in place, what stays the user's, layouts the
//! exFAT crate cannot handle refused before any write, and macOS's checkers on every volume written.

mod volume;

use switch_card::Device;
use switch_card_testing::SparseImage;
use switch_sd::{BootFile, Error, Summary, install};
use volume::{FIRST, Kind, SECTORS, fsck, get, names, put, remove, try_put};

const KINDS: [Kind; 2] = [Kind::Fat32, Kind::Exfat];

const USER_FILES: [&str; 9] = [
    "Nintendo/Contents/data.bin",
    "bootloader/hekate_ipl.ini",
    "bootloader/payloads/install-ios.bin",
    "bootloader/ini/android.ini",
    "switchroot/ios/bootargs.txt",
    "switchroot/ios/._bootargs.txt",
    "switchroot/ios/IOS17.SII.001",
    "switchroot/ios/sub/keep.txt",
    "switchroot/android/boot.img",
];

fn bytes(seed: u8, len: usize) -> Vec<u8> {
    (0..len)
        .map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed))
        .collect()
}

/// A boot package: generation 1 changes bl33.bin and drops bl31.bin.
fn package(generation: u8) -> Vec<(&'static str, Vec<u8>)> {
    let mut files = vec![
        ("switchroot/ios/bl33.bin", bytes(generation, 300_000)),
        ("switchroot/ios/bootlogo.bmp", bytes(7, 5_000)),
        (
            "bootloader/ini/ios.ini",
            b"[iOS]\nl4t=1\nboot_prefixes=/switchroot/ios/\nid=iOS\n".to_vec(),
        ),
        ("bootloader/res/icon_ios_hue.bmp", bytes(9, 3_000)),
    ];
    if generation == 0 {
        files.push(("switchroot/ios/bl31.bin", bytes(3, 70_000)));
    }
    files
}

fn run(image: &mut SparseImage, files: &[(&str, Vec<u8>)]) -> Result<Summary, Error> {
    let files: Vec<BootFile> = files.iter().map(|(path, data)| BootFile { path, data }).collect();
    install(image, FIRST, SECTORS, &files)
}

fn sorted(mut v: Vec<String>) -> Vec<String> {
    v.sort();
    v
}

#[test]
fn refuses_paths_off_the_allowlist_before_writing() {
    let mut image = volume::card(Kind::Fat32);
    let before = image.clone();
    for path in [
        "switchroot/ios/bootargs.txt",
        "SwitchRoot/IOS/BootArgs.TXT",
        "bootloader/payloads/install-ios.bin",
        "bootloader/hekate_ipl.ini",
        "switchroot/android/boot.img",
        "switchroot/ios/sub/x.bin",
        "switchroot/ios/../ios/x.bin",
        "/switchroot/ios/x.bin",
        "switchroot\\ios\\x.bin",
    ] {
        let mut files = package(0);
        files.push((path, vec![1]));
        assert!(matches!(run(&mut image, &files), Err(Error::Refused { .. })), "{path}");
        assert!(image == before, "{path}: a refused package wrote to the card");
    }
    let mut twice = package(0);
    twice.push(("switchroot/ios/BL33.BIN", vec![1]));
    assert!(matches!(
        run(&mut image, &twice),
        Err(Error::Refused {
            why: "listed twice",
            ..
        })
    ));
}

#[test]
fn replaces_the_entry_in_place_and_keeps_the_users_files() {
    for kind in KINDS {
        let mut image = volume::card(kind);
        for (i, path) in USER_FILES.iter().enumerate() {
            put(&mut image, kind, path, &bytes(i as u8, 2_000 + i * 700));
        }
        put(&mut image, kind, "switchroot/ios/old-kernel.bin", &bytes(1, 40_000));
        put(&mut image, kind, "switchroot/ios/._old-kernel.bin", &bytes(2, 4_096));

        let first = run(&mut image, &package(0)).unwrap();
        assert_eq!(first.written.len(), 5, "{kind:?}");
        let stale = ["switchroot/ios/._old-kernel.bin", "switchroot/ios/old-kernel.bin"].map(String::from);
        assert_eq!(sorted(first.removed), stale, "{kind:?}");
        for (path, data) in package(0) {
            assert_eq!(
                get(&mut image, kind, path).as_deref(),
                Some(&data[..]),
                "{kind:?} {path}"
            );
        }

        let again = run(&mut image, &package(0)).unwrap();
        assert!(
            again.written.is_empty() && again.removed.is_empty() && again.unchanged.len() == 5,
            "{kind:?}"
        );

        let next = run(&mut image, &package(1)).unwrap();
        assert_eq!(next.written, ["switchroot/ios/bl33.bin"], "{kind:?}");
        assert_eq!(next.removed, ["switchroot/ios/bl31.bin"], "{kind:?}");
        assert_eq!(
            get(&mut image, kind, "switchroot/ios/bl33.bin"),
            Some(bytes(1, 300_000)),
            "{kind:?}"
        );

        for (i, path) in USER_FILES.iter().enumerate() {
            assert_eq!(
                get(&mut image, kind, path),
                Some(bytes(i as u8, 2_000 + i * 700)),
                "{kind:?} {path}"
            );
        }
        let expected = [
            "IOS17.SII.001",
            "._bootargs.txt",
            "bl33.bin",
            "bootargs.txt",
            "bootlogo.bmp",
            "sub",
        ]
        .map(String::from);
        assert_eq!(
            names(&mut image, kind, "switchroot/ios"),
            sorted(expected.to_vec()),
            "{kind:?}"
        );
        assert!(kind == Kind::Exfat || volume::fat_copies_equal(&image));
        fsck(&image, kind, &format!("replace-{kind:?}"));
    }
}

/// The volume filled with 4 MiB files and every other one deleted, so a 10 MiB bl33.bin has to go into holes; on
/// FAT32 (512-byte clusters) switchroot/ios also starts out holding 300 stale files over many clusters.
#[test]
fn fragmented_free_space_and_a_long_entry_directory() {
    const FILL: usize = 4 << 20;
    for kind in KINDS {
        let mut image = volume::card(kind);
        let stale = if kind == Kind::Fat32 { 300 } else { 0 };
        for i in 0..stale {
            put(
                &mut image,
                kind,
                &format!("switchroot/ios/stale-{i:03}.bin"),
                &bytes(i as u8, 600),
            );
        }
        let fills = (volume::free_bytes(&mut image, kind) / FILL as u64) as usize;
        assert!(
            fills >= 8,
            "{kind:?}: {fills} fills leave a contiguous run for bl33.bin"
        );
        for i in 0..fills {
            put(
                &mut image,
                kind,
                &format!("Nintendo/fill-{i:02}.bin"),
                &bytes(i as u8, FILL),
            );
        }
        for i in (0..fills).step_by(2) {
            remove(&mut image, kind, &format!("Nintendo/fill-{i:02}.bin"));
        }
        let mut files = package(0);
        files[0].1 = bytes(5, 10 << 20);
        let summary = run(&mut image, &files).unwrap();
        assert_eq!((summary.removed.len(), summary.written.len()), (stale, 5), "{kind:?}");
        for (path, data) in &files {
            assert_eq!(
                get(&mut image, kind, path).as_deref(),
                Some(&data[..]),
                "{kind:?} {path}"
            );
        }
        for i in (1..fills).step_by(2) {
            let kept = get(&mut image, kind, &format!("Nintendo/fill-{i:02}.bin"));
            assert_eq!(kept, Some(bytes(i as u8, FILL)), "{kind:?} fill {i}");
        }
        fsck(&image, kind, &format!("fragmented-{kind:?}"));
    }
}

/// hadris-fat never grows an exFAT directory: a full switchroot/ios is an error, and the volume stays sound.
#[test]
fn exfat_full_entry_directory_is_refused_cleanly() {
    let kind = Kind::Exfat;
    let mut image = volume::card(kind);
    let mut parts = 0;
    while try_put(&mut image, kind, &format!("switchroot/ios/IOS{parts:02}.SII.001"), &[1]).is_ok() {
        parts += 1;
    }
    assert!(matches!(run(&mut image, &package(0)), Err(Error::Exfat(_))));
    assert_eq!(names(&mut image, kind, "switchroot/ios").len(), parts);
    fsck(&image, kind, "exfat-full");
}

/// The FAT entry of exFAT cluster `cluster`, set to `value`.
fn set_exfat_fat(image: &mut SparseImage, cluster: u32, value: u32) {
    let boot = image.sector(FIRST);
    let fat = FIRST + u64::from(u32::from_le_bytes(boot[80..84].try_into().unwrap()));
    let lba = fat + u64::from(cluster) * 4 / 512;
    let mut sector = image.sector(lba);
    let at = (cluster as usize * 4) % 512;
    sector[at..at + 4].copy_from_slice(&value.to_le_bytes());
    image.set(lba, &sector);
}

#[test]
fn exfat_layouts_the_crate_would_misread_are_refused_before_any_write() {
    let mut image = volume::card(Kind::Exfat);
    let boot = image.sector(FIRST);
    let root = u32::from_le_bytes(boot[96..100].try_into().unwrap());
    set_exfat_fat(&mut image, root, root + 40);
    let before = image.clone();
    assert!(matches!(run(&mut image, &package(0)), Err(Error::Unsupported(why)) if why.contains("root")));
    assert!(image == before);

    let mut image = volume::card(Kind::Exfat);
    let heap = FIRST + u64::from(u32::from_le_bytes(boot[88..92].try_into().unwrap()));
    let cluster_sectors = 1u64 << boot[109];
    let root_dir = image.sector(heap + u64::from(root - 2) * cluster_sectors);
    let bitmap = root_dir
        .chunks(32)
        .find(|e| e[0] == 0x81)
        .map(|e| u32::from_le_bytes(e[20..24].try_into().unwrap()))
        .expect("an allocation bitmap entry in the root");
    set_exfat_fat(&mut image, bitmap, bitmap + 7);
    let before = image.clone();
    assert!(matches!(run(&mut image, &package(0)), Err(Error::Unsupported(why)) if why.contains("fragmented")));
    assert!(image == before);
}

fn assert_no_case_twins(image: &mut SparseImage, kind: Kind, dir: &str) {
    let names = names(image, kind, dir);
    let mut folded: Vec<String> = names.iter().map(|n| n.to_ascii_lowercase()).collect();
    folded.sort();
    folded.dedup();
    assert_eq!(folded.len(), names.len(), "{kind:?} {dir:?}: {names:?}");
}

/// Upper-case names already on the card (FAT and exFAT compare without case) are the entry's, not twins beside it.
#[test]
fn upper_case_names_on_the_card_are_reused() {
    for kind in KINDS {
        let mut image = volume::card(kind);
        put(&mut image, kind, "BOOTLOADER/INI/IOS.INI", b"[old]\n");
        put(&mut image, kind, "BOOTLOADER/hekate_ipl.ini", b"[config]\n");
        put(&mut image, kind, "SWITCHROOT/IOS/BL33.BIN", b"old");
        put(&mut image, kind, "SWITCHROOT/IOS/BootArgs.txt", b"debug=0x14e\n");
        run(&mut image, &package(0)).unwrap();
        for (path, data) in package(0) {
            assert_eq!(
                get(&mut image, kind, path).as_deref(),
                Some(&data[..]),
                "{kind:?} {path}"
            );
        }
        assert_eq!(
            get(&mut image, kind, "switchroot/ios/bootargs.txt").as_deref(),
            Some(&b"debug=0x14e\n"[..])
        );
        assert_eq!(
            get(&mut image, kind, "bootloader/hekate_ipl.ini").as_deref(),
            Some(&b"[config]\n"[..])
        );
        for dir in ["", "bootloader", "bootloader/ini", "switchroot", "switchroot/ios"] {
            assert_no_case_twins(&mut image, kind, dir);
        }
        assert!(kind == Kind::Exfat || volume::fat_copies_equal(&image));
        fsck(&image, kind, &format!("case-{kind:?}"));
    }
}

/// switchroot/ios's stream extension made to claim two clusters (with its entry set's checksum redone): refused before
/// any write.
#[test]
fn exfat_entry_directories_longer_than_a_cluster_are_refused() {
    let kind = Kind::Exfat;
    let mut image = volume::card(kind);
    put(&mut image, kind, "switchroot/ios/bl33.bin", b"old");
    let boot = image.sector(FIRST);
    let cluster = 1u64 << boot[109];
    let heap = FIRST + u64::from(u32::from_le_bytes(boot[88..92].try_into().unwrap()));
    let root = u32::from_le_bytes(boot[96..100].try_into().unwrap());
    let switchroot = child_cluster(&image, heap + u64::from(root - 2) * cluster, cluster, "switchroot").unwrap();
    let dir = heap + u64::from(switchroot - 2) * cluster;
    let mut sectors = image.read_bytes(dir, cluster);
    let at = entry_set(&sectors, "ios").unwrap();
    let length = 2 * cluster * 512;
    sectors[at + 32 + 8..at + 32 + 16].copy_from_slice(&length.to_le_bytes());
    sectors[at + 32 + 24..at + 32 + 32].copy_from_slice(&length.to_le_bytes());
    let count = sectors[at + 1] as usize;
    let mut sum = 0u16;
    for (i, &b) in sectors[at..at + 32 * (count + 1)].iter().enumerate() {
        if i != 2 && i != 3 {
            sum = sum.rotate_right(1).wrapping_add(u16::from(b));
        }
    }
    sectors[at + 2..at + 4].copy_from_slice(&sum.to_le_bytes());
    image.set(dir, &sectors);
    let before = image.clone();
    assert!(matches!(run(&mut image, &package(0)), Err(Error::Unsupported(why)) if why.contains("longer than one")));
    assert!(image == before);
}

/// The byte offset of the entry set named `name` (ASCII) in a directory cluster's bytes.
fn entry_set(dir: &[u8], name: &str) -> Option<usize> {
    let units: Vec<u8> = name.bytes().flat_map(|b| [b, 0]).collect();
    (0..dir.len() / 32).map(|i| i * 32).find(|&at| {
        dir[at] == 0x85 && dir[at + 32] == 0xc0 && usize::from(dir[at + 32 + 3]) == name.len() && {
            let name_entry = &dir[at + 64..at + 96];
            name_entry[0] == 0xc1 && name_entry[2..2 + units.len()] == units[..]
        }
    })
}

fn child_cluster(image: &SparseImage, dir: u64, cluster: u64, name: &str) -> Option<u32> {
    let bytes = image.read_bytes(dir, cluster);
    let at = entry_set(&bytes, name)?;
    Some(u32::from_le_bytes(
        bytes[at + 32 + 20..at + 32 + 24].try_into().unwrap(),
    ))
}

struct Counting<'a> {
    inner: &'a mut SparseImage,
    reads: u64,
    read_sectors: u64,
    writes: u64,
}

impl Device for Counting<'_> {
    fn sector_size(&self) -> u32 {
        512
    }
    fn sector_count(&self) -> u64 {
        self.inner.sector_count()
    }
    fn read_sectors(&mut self, lba: u64, buf: &mut [u8]) -> std::io::Result<()> {
        self.reads += 1;
        self.read_sectors += (buf.len() / 512) as u64;
        self.inner.read_sectors(lba, buf)
    }
    fn write_sectors(&mut self, lba: u64, buf: &[u8]) -> std::io::Result<()> {
        self.writes += 1;
        self.inner.write_sectors(lba, buf)
    }
    fn barrier(&mut self) -> std::io::Result<()> {
        self.inner.barrier()
    }
}

/// Device transfers for one install on a fresh 32 GiB FAT32 volume with 32 KiB clusters, whose FAT is 8 MiB: the
/// counts go to stderr, and the reads stay below one pass over the FAT.
#[test]
fn reads_for_one_install_on_a_32_gib_fat32() {
    const VOLUME: u64 = 32 << 21;
    let path = volume::scratch_image();
    volume::format(Kind::Fat32, &path, VOLUME, 64);
    let mut boot = [0u8; 512];
    std::io::Read::read_exact(&mut std::fs::File::open(&path).unwrap(), &mut boot).unwrap();
    let reserved = u64::from(u16::from_le_bytes([boot[14], boot[15]]));
    let fat = u64::from(u32::from_le_bytes(boot[36..40].try_into().unwrap()));
    let mut image = volume::on_card(&path, FIRST + VOLUME + 2048, reserved + 2 * fat + 64);
    std::fs::remove_file(path).unwrap();
    let files = package(0);
    let boot_files: Vec<BootFile> = files.iter().map(|(path, data)| BootFile { path, data }).collect();
    let mut dev = Counting {
        inner: &mut image,
        reads: 0,
        read_sectors: 0,
        writes: 0,
    };
    install(&mut dev, FIRST, VOLUME, &boot_files).unwrap();
    eprintln!(
        "32 GiB FAT32 ({fat} FAT sectors): {} reads of {} sectors, {} writes",
        dev.reads, dev.read_sectors, dev.writes
    );
    assert!(dev.read_sectors < fat, "{} sectors read", dev.read_sectors);
}
