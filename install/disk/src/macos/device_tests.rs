// SPDX-License-Identifier: GPL-3.0-or-later
//! On disk images attached to this Mac: the real /dev/disk path, DiskArbitration and IOKit, never a real disk.
//! Each test attaches its own images and detaches only those.
use std::fs::{File, OpenOptions as FsOpenOptions};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::Mutex;

use super::{Arbiter, iokit, mounts};
use crate::{Disk, Error, OpenOptions, Privilege, RawDisk, Refusal};

const MIB: u64 = 1 << 20;
static SERIAL: Mutex<()> = Mutex::new(());

struct Image {
    _dir: tempfile::TempDir,
    path: PathBuf,
    bsd: String,
    dev: String,
    root: u64,
}

impl Image {
    fn attach(mib: u64) -> Image {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("card.img");
        File::create(&path).unwrap().set_len(mib * MIB).unwrap();
        let (bsd, root) = attach_raw(&path, &["-nomount"]);
        Image {
            _dir: dir,
            path,
            dev: format!("/dev/{bsd}"),
            bsd,
            root,
        }
    }

    /// Detaches the image and attaches it again the way a user's double-click would, so DiskArbitration mounts its
    /// volumes and mounts them again whenever it probes them anew.
    fn reattach_public(&mut self) {
        run("/usr/bin/hdiutil", &["detach", &self.dev]);
        let (bsd, root) = attach_raw(&self.path, &[]);
        self.dev = format!("/dev/{bsd}");
        self.bsd = bsd;
        self.root = root;
    }

    fn partition(&self, layout: &[&str]) {
        let mut args = vec!["partitionDisk", self.dev.as_str(), "GPT"];
        args.extend_from_slice(layout);
        run("/usr/sbin/diskutil", &args);
    }

    fn dev(&self) -> PathBuf {
        PathBuf::from(&self.dev)
    }

    fn listed(&self) -> Disk {
        crate::list()
            .unwrap()
            .into_iter()
            .find(|d| d.path == self.dev())
            .expect("the image is listed")
    }

    fn root(&self) -> u64 {
        self.root
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        if iokit::media_by_bsd(&self.bsd).is_some_and(|m| m.id == self.root) {
            let _ = Command::new("/usr/bin/hdiutil")
                .args(["detach", "-force", &self.dev])
                .output();
        }
    }
}

fn attach_raw(path: &Path, extra: &[&str]) -> (String, u64) {
    let mut args = vec!["attach", "-imagekey", "diskimage-class=CRawDiskImage"];
    args.extend_from_slice(extra);
    args.push(path.to_str().unwrap());
    let out = run("/usr/bin/hdiutil", &args);
    let text = String::from_utf8_lossy(&out.stdout);
    let bsd = text
        .split_whitespace()
        .find_map(|w| w.strip_prefix("/dev/"))
        .expect("hdiutil names the disk")
        .to_owned();
    let root = iokit::media_by_bsd(&bsd).expect("IOKit has the image").id;
    (bsd, root)
}

fn run(tool: &str, args: &[&str]) -> Output {
    let out = Command::new(tool).args(args).output().unwrap();
    assert!(
        out.status.success(),
        "{tool} {args:?}: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

fn succeeds(tool: &str, args: &[&str]) -> bool {
    Command::new(tool).args(args).output().unwrap().status.success()
}

fn name(prefix: &str) -> String {
    format!("{prefix}{}", std::process::id() % 100_000)
}

fn mounted_on(disk: &Disk) -> usize {
    disk.mounted().count()
}

/// The free space diskutil leaves after the given partitions: where the write tests go, away from any file system.
fn free_start(disk: &Disk) -> u64 {
    disk.partitions.iter().map(|p| p.offset + p.size).max().unwrap() + MIB
}

fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len)
        .map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed))
        .collect()
}

fn read(raw: &RawDisk, offset: u64, len: usize) -> Vec<u8> {
    let mut buf = vec![0u8; len];
    raw.read_at(offset, &mut buf).unwrap();
    buf
}

#[test]
fn list_maps_apfs_volumes_to_their_partition_and_refuses_the_boot_disk() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let image = Image::attach(256);
    image.partition(&[
        "MS-DOS",
        &name("DF"),
        "64M",
        "APFS",
        &name("DA"),
        "128M",
        "Free Space",
        "gap",
        "R",
    ]);
    let disk = image.listed();
    assert_eq!(disk.refusal(), None, "{disk:?}");
    assert_eq!(disk.bus, crate::Bus::Virtual);
    assert_eq!(disk.partitions.len(), 2, "{disk:?}");
    assert_eq!(disk.partitions[0].volumes.len(), 1, "FAT mounted: {disk:?}");
    assert_eq!(
        disk.partitions[1].volumes.len(),
        1,
        "APFS volume on its partition: {disk:?}"
    );
    assert_eq!(disk.partitions[1].volumes[0].file_system, "apfs");

    let disks = crate::list().unwrap();
    let boot = disks
        .iter()
        .find(|d| d.mounted().any(|v| v.mount_point == Path::new("/")))
        .expect("the boot disk");
    assert_eq!(boot.refusal(), Some(Refusal::System), "{boot:?}");

    let stale = Disk {
        id: "macos:1".into(),
        ..disk
    };
    assert!(matches!(
        crate::open(&stale, &OpenOptions::default()),
        Err(Error::Changed(_))
    ));
}

#[test]
fn hold_unmounts_refuses_writes_and_restores() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let image = Image::attach(256);
    image.partition(&[
        "MS-DOS",
        &name("HF"),
        "64M",
        "APFS",
        &name("HA"),
        "128M",
        "Free Space",
        "gap",
        "R",
    ]);
    let disk = image.listed();
    assert_eq!(mounted_on(&disk), 2);
    let fat = disk.partitions[0].path.clone();
    let start = free_start(&disk);

    let before: Vec<PathBuf> = disk.mounted().map(|v| v.mount_point.clone()).collect();
    let raw = crate::open(&disk, &OpenOptions::default()).unwrap();
    let now: Vec<PathBuf> = mounts::mounts().unwrap().into_iter().map(|m| m.mount_point).collect();
    assert!(
        before.iter().all(|m| !now.contains(m)),
        "nothing on it stays mounted: {before:?} in {now:?}"
    );
    assert!(
        !iokit::all_media()
            .unwrap()
            .iter()
            .any(|m| m.is_on(image.root) && m.id != image.root),
        "no media left above it"
    );
    assert!(!fat.exists(), "the exclusive open tears the partitions down");
    let busy = FsOpenOptions::new()
        .read(true)
        .write(true)
        .open(format!("/dev/r{}", image.bsd))
        .unwrap_err();
    assert_eq!(busy.raw_os_error(), Some(libc::EBUSY));
    assert!(!succeeds("/usr/sbin/diskutil", &["mount", fat.to_str().unwrap()]));
    assert_eq!(raw.size(), disk.size);
    assert_eq!(raw.logical_sector(), 512);

    let mut aligned = raw.buffer(MIB as usize);
    aligned.copy_from_slice(&pattern(MIB as usize, 1));
    raw.write_at(start, &aligned).unwrap();
    assert_eq!(read(&raw, start, MIB as usize), &aligned[..]);

    let small = pattern(5001, 7);
    raw.write_at(start + 1000, &small[1..]).unwrap();
    let around = read(&raw, start, 8192);
    assert_eq!(&around[1000..6000], &small[1..]);
    assert_eq!(
        &around[..1000],
        &aligned[..1000],
        "the head sector keeps its other bytes"
    );
    assert_eq!(
        &around[6000..],
        &aligned[6000..8192],
        "the tail sector keeps its other bytes"
    );

    let big = pattern(9 * MIB as usize + 1, 3);
    raw.write_at(start + 2 * MIB, &big[1..]).unwrap();
    assert!(
        read(&raw, start + 2 * MIB, 9 * MIB as usize) == big[1..],
        "a misaligned buffer goes through staging"
    );

    let mut writer = raw.writer(start + 12 * MIB + 3).unwrap();
    let pieces: Vec<Vec<u8>> = [1usize, 7, 513, 4096, 100_000]
        .iter()
        .map(|&n| pattern(n, n as u8))
        .collect();
    for piece in &pieces {
        std::io::Write::write_all(&mut writer, piece).unwrap();
    }
    let end = writer.finish().unwrap();
    let joined: Vec<u8> = pieces.concat();
    assert_eq!(end, start + 12 * MIB + 3 + joined.len() as u64);
    assert_eq!(read(&raw, start + 12 * MIB + 3, joined.len()), joined);
    raw.flush().unwrap();

    raw.release().unwrap();
    let after = image.listed();
    assert_eq!(after.partitions.len(), 2, "{after:?}");
    assert_eq!(mounted_on(&after), 2, "both volumes mounted again: {after:?}");
}

#[test]
fn mount_refusal_covers_partitions_and_apfs_volumes_of_its_disk_only() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let held = Image::attach(256);
    held.partition(&["MS-DOS", &name("GF"), "64M", "APFS", &name("GA"), "R"]);
    let other = Image::attach(128);
    other.partition(&["MS-DOS", &name("OF"), "R"]);
    let volumes: Vec<String> = mounts::mounts()
        .unwrap()
        .into_iter()
        .filter(|m| iokit::media_by_bsd(&m.bsd).is_some_and(|x| x.is_on(held.root()) || x.is_on(other.root())))
        .map(|m| m.bsd)
        .collect();
    assert_eq!(volumes.len(), 3, "{volumes:?}");
    for v in &volumes {
        run("/usr/sbin/diskutil", &["unmount", v]);
    }

    let arbiter = Arbiter::hold(held.root()).unwrap();
    for v in &volumes {
        let ours = iokit::media_by_bsd(v).unwrap().is_on(held.root());
        assert_eq!(succeeds("/usr/sbin/diskutil", &["mount", v]), !ours, "{v}");
    }
    let refused = arbiter.refused();
    for v in volumes
        .iter()
        .filter(|v| iokit::media_by_bsd(v).unwrap().is_on(held.root()))
    {
        assert!(refused.contains(v), "{v} refused: {refused:?}");
    }
    assert!(
        refused.iter().any(|r| !r.starts_with(&held.bsd)),
        "the APFS volume, on the synthesized disk: {refused:?}"
    );
    drop(arbiter);
    for v in volumes
        .iter()
        .filter(|v| iokit::media_by_bsd(v).unwrap().is_on(held.root()))
    {
        run("/usr/sbin/diskutil", &["mount", v]);
    }
}

#[test]
fn authopen_gives_the_same_hold() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let image = Image::attach(128);
    image.partition(&["MS-DOS", &name("AF"), "64M", "Free Space", "gap", "R"]);
    let disk = image.listed();
    let options = OpenOptions {
        force_unmount: false,
        privilege: Privilege::Helper,
    };
    let raw = crate::open(&disk, &options).unwrap();
    assert!(!disk.partitions[0].path.exists());
    let busy = FsOpenOptions::new()
        .read(true)
        .write(true)
        .open(format!("/dev/r{}", image.bsd))
        .unwrap_err();
    assert_eq!(busy.raw_os_error(), Some(libc::EBUSY));
    let start = free_start(&disk);
    raw.write_at(start + 3, b"authopen").unwrap();
    assert_eq!(read(&raw, start + 3, 8), b"authopen");
    raw.release().unwrap();
    assert_eq!(mounted_on(&image.listed()), 1);
}

#[test]
fn eject_detaches_the_disk_with_mounts_still_refused() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let image = Image::attach(256);
    image.partition(&[
        "MS-DOS",
        &name("EF"),
        "64M",
        "APFS",
        &name("EA"),
        "128M",
        "Free Space",
        "gap",
        "R",
    ]);
    let disk = image.listed();
    let root = image.root();
    let raw = crate::open(&disk, &OpenOptions::default()).unwrap();
    raw.write_at(free_start(&disk), &[0xa5; 4096]).unwrap();
    raw.eject().unwrap();
    assert!(
        !iokit::all_media().unwrap().iter().any(|m| m.is_on(root)),
        "ejected: no media left on it"
    );
}

#[test]
fn diskarbitrations_own_mount_after_close_meets_the_refusal() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut image = Image::attach(256);
    image.partition(&[
        "MS-DOS",
        &name("PF"),
        "64M",
        "APFS",
        &name("PA"),
        "128M",
        "Free Space",
        "gap",
        "R",
    ]);
    image.reattach_public();
    let disk = image.listed();
    assert_eq!(
        mounted_on(&disk),
        2,
        "DiskArbitration mounted the public attach: {disk:?}"
    );

    let (file, _, hold) = super::open(&disk, &OpenOptions::default()).unwrap();
    hold.close_and_settle(file).unwrap();
    let refused_by_settle = hold.arbiter.refused();
    let mounted_while_refusing = mounted_on(&image.listed());
    hold.restore().unwrap();
    let after = image.listed();
    println!(
        "refused before the settle returned: {refused_by_settle:?}; mounted while refusing: {mounted_while_refusing}; \
         mounted after restore: {}",
        mounted_on(&after)
    );
    assert_eq!(mounted_while_refusing, 0);
    let volumes: Vec<String> = iokit::all_media()
        .unwrap()
        .into_iter()
        .filter(|m| m.is_on(image.root) && m.id != image.root && !m.content.contains("partition_scheme"))
        .map(|m| m.bsd)
        .collect();
    for v in mounts::mounts().unwrap().iter().filter(|m| volumes.contains(&m.bsd)) {
        assert!(
            refused_by_settle.contains(&v.bsd),
            "{} was asked for before the settle returned: {refused_by_settle:?}",
            v.bsd
        );
    }
    assert_eq!(mounted_on(&after), 2, "{after:?}");
}
