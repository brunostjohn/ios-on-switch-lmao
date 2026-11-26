// SPDX-License-Identifier: GPL-3.0-or-later
//! macOS's own FAT32 and exFAT tools on card images: `newfs_msdos`/`newfs_exfat` make the volumes,
//! `fsck_msdos -n`/`fsck_exfat -n` check them.

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};

use switch_card_testing::SparseImage;

pub fn scratch(name: &str) -> PathBuf {
    static MADE: AtomicUsize = AtomicUsize::new(0);
    let n = MADE.fetch_add(1, Ordering::Relaxed);
    PathBuf::from(std::env::var("TEST_TMPDIR").unwrap()).join(format!("{n}-{name}"))
}

fn run(tool: &str, args: &[&str]) -> (bool, String) {
    let out = Command::new(tool).args(args).output().unwrap();
    let text = String::from_utf8_lossy(&out.stdout).into_owned() + &String::from_utf8_lossy(&out.stderr);
    (out.status.success(), text)
}

struct Attached(String);

impl Attached {
    fn open(path: &Path) -> Attached {
        let out = Command::new("/usr/bin/hdiutil")
            .args([
                "attach",
                "-imagekey",
                "diskimage-class=CRawDiskImage",
                "-nomount",
                path.to_str().unwrap(),
            ])
            .output()
            .unwrap();
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(out.status.success(), "hdiutil attach: {stdout}{stderr}");
        let dev = stdout
            .split_whitespace()
            .next()
            .unwrap_or_else(|| panic!("hdiutil attach returned no device: {stdout}{stderr}"))
            .to_string();
        Attached(dev)
    }

    fn dev(&self) -> &str {
        &self.0
    }
}

impl Drop for Attached {
    fn drop(&mut self) {
        let (ok, _) = run("/usr/bin/hdiutil", &["detach", &self.0]);
        if !ok {
            let _ = run("/usr/bin/hdiutil", &["detach", "-force", &self.0]);
        }
    }
}

fn load_range(image: &mut SparseImage, file: &mut File, dst_base: u64, src_sector: u64, count: u64) {
    file.seek(SeekFrom::Start(src_sector * 512)).unwrap();
    let mut chunk = vec![0u8; 1 << 20];
    let mut done = 0;
    while done < count {
        let n = (((count - done) * 512) as usize).min(chunk.len());
        file.read_exact(&mut chunk[..n]).unwrap();
        for (i, sector) in chunk[..n].as_chunks::<512>().0.iter().enumerate() {
            if sector.iter().any(|&b| b != 0) {
                image.set(dst_base + src_sector + done + i as u64, sector);
            }
        }
        done += (n / 512) as u64;
    }
}

/// A FAT32 volume `newfs_msdos` makes, `sectors` long with `cluster` sectors a cluster, put into `image` at `first`:
/// its reserved sectors, FATs and root cluster, which is all a fresh volume holds.
pub fn format_fat32(image: &mut SparseImage, first: u64, sectors: u64, cluster: u32) {
    let path = scratch("newfs.img");
    File::create(&path).unwrap().set_len(sectors * 512).unwrap();
    let (s, c, o) = (sectors.to_string(), cluster.to_string(), first.to_string());
    let p = path.to_str().unwrap();
    let args = [
        "-F", "32", "-c", &c, "-S", "512", "-o", &o, "-h", "64", "-u", "32", "-s", &s, p,
    ];
    let (ok, text) = run("/sbin/newfs_msdos", &args);
    assert!(ok, "newfs_msdos: {text}");
    let mut file = File::open(&path).unwrap();
    let mut boot = [0u8; 512];
    file.read_exact(&mut boot).unwrap();
    let u16_at = |at: usize| u64::from(u16::from_le_bytes([boot[at], boot[at + 1]]));
    let fat = u64::from(u32::from_le_bytes(boot[36..40].try_into().unwrap()));
    let load = u16_at(14) + u64::from(boot[16]) * fat + u64::from(boot[13]);
    load_range(image, &mut file, first, 0, load);
    drop(file);
    std::fs::remove_file(path).unwrap();
}

fn exfat_boot_checksum(sectors: &[u8]) -> u32 {
    let mut sum = 0u32;
    for (i, &b) in sectors.iter().enumerate() {
        if i == 106 || i == 107 || i == 112 {
            continue;
        }
        sum = sum.rotate_right(1).wrapping_add(u32::from(b));
    }
    sum
}

/// An exFAT volume `newfs_exfat` makes via `hdiutil`, `sectors` long with `cluster` sectors a cluster, put into
/// `image` at `first` with `PartitionOffset = first`.
pub fn format_exfat(image: &mut SparseImage, first: u64, sectors: u64, cluster: u32) {
    let path = scratch("newfs-exfat.img");
    File::create(&path).unwrap().set_len(sectors * 512).unwrap();
    {
        let attached = Attached::open(&path);
        let c = cluster.to_string();
        let (ok, text) = run("/sbin/newfs_exfat", &["-c", &c, attached.dev()]);
        assert!(ok, "newfs_exfat: {text}");
    }
    let mut file = File::open(&path).unwrap();
    let mut main_region = vec![0u8; 12 * 512];
    file.read_exact(&mut main_region).unwrap();
    main_region[64..72].copy_from_slice(&first.to_le_bytes());
    let sum = exfat_boot_checksum(&main_region[..11 * 512]).to_le_bytes();
    for slot in main_region[11 * 512..12 * 512].as_chunks_mut::<4>().0 {
        slot.copy_from_slice(&sum);
    }
    image.set(first, &main_region);
    image.set(first + 12, &main_region);

    let u32_at = |at: usize| u32::from_le_bytes(main_region[at..at + 4].try_into().unwrap());
    let fat_off = u64::from(u32_at(80));
    let fat_len = u64::from(u32_at(84));
    let heap_off = u64::from(u32_at(88));
    let root = u32_at(96);
    let spc = 1u64 << main_region[109];
    let cb = spc * 512;

    load_range(image, &mut file, first, fat_off, fat_len);
    let root_sec = heap_off + u64::from(root - 2) * spc;
    load_range(image, &mut file, first, root_sec, spc);

    let root_bytes = image.read_bytes(first + root_sec, spc);
    for e in root_bytes.as_chunks::<32>().0 {
        if e[0] == 0 {
            break;
        }
        if e[0] == 0x81 || e[0] == 0x82 {
            let start = u32::from_le_bytes(e[20..24].try_into().unwrap());
            let len = u64::from_le_bytes(e[24..32].try_into().unwrap());
            let clus = len.div_ceil(cb);
            let sec = heap_off + u64::from(start - 2) * spc;
            load_range(image, &mut file, first, sec, clus * spc);
        }
    }
    drop(file);
    std::fs::remove_file(path).unwrap();
}

/// The volume at `first..first+sectors` of `image` as its own sparse image file.
fn export(image: &SparseImage, first: u64, sectors: u64, name: &str) -> PathBuf {
    let path = scratch(&format!("{name}.img"));
    let mut file = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(&path)
        .unwrap();
    file.set_len(sectors * 512).unwrap();
    for (lba, sector) in image.data.range(first..first + sectors) {
        file.seek(SeekFrom::Start((lba - first) * 512)).unwrap();
        file.write_all(&sector[..]).unwrap();
    }
    path
}

/// `fsck_msdos -n` on the volume at `first..first+sectors`: its output when clean, a panic otherwise.
pub fn fsck_fat32(image: &SparseImage, first: u64, sectors: u64, name: &str) -> String {
    let path = export(image, first, sectors, name);
    let (ok, text) = run("/sbin/fsck_msdos", &["-n", path.to_str().unwrap()]);
    std::fs::remove_file(&path).unwrap();
    assert!(ok, "{name}: fsck_msdos -n failed:\n{text}");
    assert!(
        !text.contains("orrect") && !text.contains("ost cluster") && !text.contains("ross-link"),
        "{name}: fsck_msdos -n found something:\n{text}"
    );
    text
}

/// `fsck_exfat -n` on the volume at `first..first+sectors`: its output when clean, a panic otherwise.
pub fn fsck_exfat(image: &SparseImage, first: u64, sectors: u64, name: &str) -> String {
    let path = export(image, first, sectors, name);
    let (ok, text) = {
        let attached = Attached::open(&path);
        run("/sbin/fsck_exfat", &["-n", attached.dev()])
    };
    std::fs::remove_file(&path).unwrap();
    assert!(ok, "{name}: fsck_exfat -n failed:\n{text}");
    assert!(
        !text.contains("corrupt") && !text.contains("needs to be repaired") && !text.contains("Update?"),
        "{name}: fsck_exfat -n found something:\n{text}"
    );
    text
}
