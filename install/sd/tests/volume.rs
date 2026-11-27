// SPDX-License-Identifier: GPL-3.0-or-later
//! Card images whose boot volume macOS formatted (`newfs_msdos` on a file, `newfs_exfat` on an attached image), files
//! put on it and read back through hadris-fat, and macOS's own checkers run on it. (Workers attach images but cannot
//! mount them, so macOS never writes files into one.)

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};

use hadris_fat::exfat::{ExFatDir, ExFatVolume};
use hadris_fat::{FatDir, FatVolume, FatVolumeWriteExt};
use switch_card::BASIC_DATA_TYPE;
use switch_card_testing::{SparseImage, tables};
use switch_sd::VolumeStream;

pub const FIRST: u64 = 2048;
pub const SECTORS: u64 = 131_072;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Fat32,
    Exfat,
}

type Stream<'a> = VolumeStream<'a, SparseImage>;

fn scratch(name: &str) -> PathBuf {
    Path::new(&std::env::var("TEST_TMPDIR").unwrap()).join(name)
}

fn run(tool: &str, args: &[&str]) -> (bool, String) {
    let out = Command::new(tool).args(args).output().unwrap();
    let text = String::from_utf8_lossy(&out.stdout).into_owned() + &String::from_utf8_lossy(&out.stderr);
    (out.status.success(), text)
}

/// `body` with the image file at `path` attached, unmounted, as a raw disk: the exFAT tools want a device.
fn attached<T>(path: &Path, body: impl FnOnce(&str) -> T) -> T {
    let path = path.to_str().unwrap();
    let (ok, text) = run(
        "/usr/bin/hdiutil",
        &["attach", "-nomount", "-imagekey", "diskimage-class=CRawDiskImage", path],
    );
    assert!(ok, "hdiutil attach: {text}");
    let disk = text
        .split_whitespace()
        .find(|w| w.starts_with("/dev/disk"))
        .unwrap()
        .to_string();
    let result = body(&disk.replace("/dev/disk", "/dev/rdisk"));
    let (ok, text) = run("/usr/bin/hdiutil", &["detach", &disk]);
    assert!(ok, "hdiutil detach: {text}");
    result
}

/// Formats the image file at `path` (`sectors` long) as `kind` with `cluster` sectors per cluster.
pub fn format(kind: Kind, path: &Path, sectors: u64, cluster: u32) {
    std::fs::File::create(path).unwrap().set_len(sectors * 512).unwrap();
    let (sectors, cluster) = (sectors.to_string(), cluster.to_string());
    let path_str = path.to_str().unwrap();
    let (ok, text) = match kind {
        Kind::Fat32 => run(
            "/sbin/newfs_msdos",
            &[
                "-F", "32", "-c", &cluster, "-S", "512", "-o", "0", "-h", "64", "-u", "32", "-s", &sectors, path_str,
            ],
        ),
        Kind::Exfat => attached(path, |dev| run("/sbin/newfs_exfat", &["-c", &cluster, dev])),
    };
    assert!(ok, "formatting {kind:?}: {text}");
}

/// A card image of `card_sectors` whose GPT index 0 / MBR slot 0 is the volume file at `path`, its first `load`
/// sectors copied in.
pub fn on_card(path: &Path, card_sectors: u64, load: u64) -> SparseImage {
    let volume = std::fs::metadata(path).unwrap().len() / 512;
    let entries = [(0, tables::entry(BASIC_DATA_TYPE, 1, "hos_data", FIRST, volume))];
    let mut image = tables::card(card_sectors, 128, 4, &entries);
    let mut file = std::fs::File::open(path).unwrap();
    let mut chunk = vec![0; 1 << 20];
    let mut lba = 0;
    while lba < load {
        let n = (((load - lba) * 512) as usize).min(chunk.len());
        file.read_exact(&mut chunk[..n]).unwrap();
        for (i, sector) in chunk[..n].chunks(512).enumerate() {
            if sector.iter().any(|&b| b != 0) {
                image.set(FIRST + lba + i as u64, sector);
            }
        }
        lba += (n / 512) as u64;
    }
    image
}

pub fn scratch_image() -> PathBuf {
    static MADE: AtomicUsize = AtomicUsize::new(0);
    scratch(&format!("volume-{}.img", MADE.fetch_add(1, Ordering::Relaxed)))
}

/// A card whose GPT index 0 / MBR slot 0 is a fresh volume of `kind`, 512-byte clusters for FAT32 and 4 KiB for
/// exFAT.
pub fn card(kind: Kind) -> SparseImage {
    let path = scratch_image();
    format(kind, &path, SECTORS, if kind == Kind::Fat32 { 1 } else { 8 });
    let image = on_card(&path, 1 << 19, SECTORS);
    std::fs::remove_file(path).unwrap();
    image
}

/// FAT32's copies of the table are byte-equal.
pub fn fat_copies_equal(image: &SparseImage) -> bool {
    let boot = image.sector(FIRST);
    let reserved = u64::from(u16::from_le_bytes([boot[14], boot[15]]));
    let size = u64::from(u32::from_le_bytes(boot[36..40].try_into().unwrap()));
    assert_eq!(boot[16], 2);
    let first = FIRST + reserved;
    image.read_bytes(first, size) == image.read_bytes(first + size, size)
}

fn stream(image: &mut SparseImage) -> Stream<'_> {
    VolumeStream::new(image, FIRST, SECTORS).unwrap()
}

fn fat_dir<'v, 'a>(vol: &'v FatVolume<Stream<'a>>, parent: &str) -> FatDir<'v, Stream<'a>> {
    let mut dir = vol.root_dir();
    for c in parent.split('/').filter(|c| !c.is_empty()) {
        dir = match dir.find(c).unwrap() {
            Some(_) => dir.open_dir(c).unwrap(),
            None => vol.create_dir(&dir, c).unwrap(),
        };
    }
    dir
}

fn exfat_dir<'v, 'a>(vol: &'v ExFatVolume<Stream<'a>>, parent: &str) -> ExFatDir<'v, Stream<'a>> {
    let mut dir = vol.root_dir();
    for c in parent.split('/').filter(|c| !c.is_empty()) {
        dir = match dir.find(c).unwrap() {
            Some(_) => dir.open_dir(c).unwrap(),
            None => vol.create_dir(&dir, c).unwrap(),
        };
    }
    dir
}

/// Writes `path`; Err with the crate's message when it cannot (an exFAT directory full, say).
pub fn try_put(image: &mut SparseImage, kind: Kind, path: &str, data: &[u8]) -> Result<(), String> {
    let (parent, name) = path.rsplit_once('/').unwrap_or(("", path));
    match kind {
        Kind::Fat32 => {
            let vol = FatVolume::open(stream(image)).unwrap();
            let dir = fat_dir(&vol, parent);
            let entry = vol.create_file(&dir, name).map_err(|x| x.to_string())?;
            let mut writer = vol.write_file(&entry).unwrap();
            let mut rest = data;
            while !rest.is_empty() {
                rest = &rest[writer.write(rest).map_err(|x| x.to_string())?..];
            }
            writer.finish().map_err(|x| x.to_string())?;
            vol.sync().unwrap();
        }
        Kind::Exfat => {
            let vol = ExFatVolume::open(stream(image)).unwrap();
            let dir = exfat_dir(&vol, parent);
            let entry = vol.create_file(&dir, name).map_err(|x| x.to_string())?;
            let mut writer = vol.write_file(&entry).unwrap();
            hadris_fat::io::Write::write_all(&mut writer, data).map_err(|x| x.to_string())?;
            writer.finish().map_err(|x| x.to_string())?;
            vol.sync_bitmap().unwrap();
        }
    }
    Ok(())
}

pub fn put(image: &mut SparseImage, kind: Kind, path: &str, data: &[u8]) {
    try_put(image, kind, path, data).unwrap_or_else(|e| panic!("{path}: {e}"));
}

pub fn remove(image: &mut SparseImage, kind: Kind, path: &str) {
    match kind {
        Kind::Fat32 => {
            let vol = FatVolume::open(stream(image)).unwrap();
            vol.delete(&vol.open_path(path).unwrap()).unwrap();
            vol.sync().unwrap();
        }
        Kind::Exfat => {
            let vol = ExFatVolume::open(stream(image)).unwrap();
            vol.delete(&vol.open_path(path).unwrap()).unwrap();
            vol.sync_bitmap().unwrap();
        }
    }
}

pub fn get(image: &mut SparseImage, kind: Kind, path: &str) -> Option<Vec<u8>> {
    match kind {
        Kind::Fat32 => {
            let vol = FatVolume::open(stream(image)).unwrap();
            let mut reader = vol.open_file_path(path).ok()?;
            Some(reader.read_to_vec().unwrap())
        }
        Kind::Exfat => {
            let vol = ExFatVolume::open(stream(image)).unwrap();
            let mut reader = vol.open_file(path).ok()?;
            let mut data = vec![0; reader.size() as usize];
            hadris_fat::io::Read::read_exact(&mut reader, &mut data).unwrap();
            Some(data)
        }
    }
}

pub fn free_bytes(image: &mut SparseImage, kind: Kind) -> u64 {
    let boot = image.sector(FIRST);
    match kind {
        Kind::Fat32 => {
            let vol = FatVolume::open(stream(image)).unwrap();
            u64::from(vol.free_cluster_count().unwrap()) * u64::from(boot[13]) * 512
        }
        Kind::Exfat => {
            let vol = ExFatVolume::open(stream(image)).unwrap();
            u64::from(vol.free_cluster_count()) << (boot[108] + boot[109])
        }
    }
}

pub fn names(image: &mut SparseImage, kind: Kind, dir: &str) -> Vec<String> {
    let mut names: Vec<String> = match kind {
        Kind::Fat32 => {
            let vol = FatVolume::open(stream(image)).unwrap();
            let dir = if dir.is_empty() {
                vol.root_dir()
            } else {
                vol.open_dir_path(dir).unwrap()
            };
            dir.entries()
                .filter_map(|e| e.unwrap().as_entry().map(|e| e.name().into_owned()))
                .filter(|n| n != "." && n != "..")
                .collect()
        }
        Kind::Exfat => {
            let vol = ExFatVolume::open(stream(image)).unwrap();
            let dir = if dir.is_empty() {
                vol.root_dir()
            } else {
                vol.open_dir(dir).unwrap()
            };
            dir.entries().map(|e| e.unwrap().name).collect()
        }
    };
    names.sort();
    names
}

/// `fsck_msdos -n` on the volume written out as its own image file, or `fsck_exfat -n` on that file attached.
pub fn fsck(image: &SparseImage, kind: Kind, name: &str) {
    let path = scratch(&format!("{name}.img"));
    std::fs::write(&path, image.read_bytes(FIRST, SECTORS)).unwrap();
    let (ok, text) = match kind {
        Kind::Fat32 => run("/sbin/fsck_msdos", &["-n", path.to_str().unwrap()]),
        Kind::Exfat => attached(&path, |dev| run("/sbin/fsck_exfat", &["-n", dev])),
    };
    eprintln!("{name}: fsck -n:\n{text}");
    assert!(ok, "{name}: fsck -n failed:\n{text}");
    std::fs::remove_file(path).unwrap();
}
