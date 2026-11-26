// SPDX-License-Identifier: GPL-3.0-or-later
//! macOS's own FAT tools on card images: `newfs_msdos` makes the volumes, `fsck_msdos -n` checks them.

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;
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
    file.seek(SeekFrom::Start(0)).unwrap();
    let mut chunk = vec![0u8; 1 << 20];
    let mut lba = 0;
    while lba < load {
        let n = (((load - lba) * 512) as usize).min(chunk.len());
        file.read_exact(&mut chunk[..n]).unwrap();
        for (i, sector) in chunk[..n].chunks(512).enumerate() {
            if sector.iter().any(|&b| b != 0) {
                image.set(first + lba + i as u64, sector);
            }
        }
        lba += (n / 512) as u64;
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
