// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::os::unix::fs::symlink;
use std::path::Path;

use super::sysfs::*;
use crate::model::Bus;

fn write(path: &Path, text: &str) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, text).unwrap();
}

fn device(sys: &Path, real: &str, name: &str, dev: &str, size: u64, removable: bool) -> std::path::PathBuf {
    let dir = sys.join("devices").join(real).join("block").join(name);
    write(&dir.join("dev"), &format!("{dev}\n"));
    write(&dir.join("size"), &format!("{size}\n"));
    write(&dir.join("removable"), if removable { "1\n" } else { "0\n" });
    write(&dir.join("ro"), "0\n");
    write(&dir.join("queue/logical_block_size"), "512\n");
    write(&dir.join("queue/physical_block_size"), "4096\n");
    fs::create_dir_all(sys.join("block")).unwrap();
    symlink(&dir, sys.join("block").join(name)).unwrap();
    dir
}

#[test]
fn usb_reader_with_two_partitions() {
    let tmp = tempfile::tempdir().unwrap();
    let sys = tmp.path();
    let dir = device(
        sys,
        "pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host6/target6:0:0/6:0:0:0",
        "sdb",
        "8:16",
        1 << 21,
        true,
    );
    write(&dir.join("device/vendor"), "Generic \n");
    write(&dir.join("device/model"), "STORAGE DEVICE  \n");
    write(&dir.join("diskseq"), "17\n");
    for (n, start, label) in [(1, 2048, "hos_data"), (2, 1 << 20, "iOS")] {
        let p = dir.join(format!("sdb{n}"));
        write(&p.join("partition"), &format!("{n}\n"));
        write(&p.join("start"), &format!("{start}\n"));
        write(&p.join("size"), "4096\n");
        write(&p.join("dev"), &format!("8:{}\n", 16 + n));
        write(
            &p.join("uevent"),
            &format!("MAJOR=8\nMINOR={}\nDEVNAME=sdb{n}\nPARTNAME={label}\n", 16 + n),
        );
    }
    device(sys, "virtual/block", "loop3", "7:3", 0, false);
    device(sys, "virtual/block", "dm-0", "253:0", 100, false);

    let devices = read_block_devices(sys).unwrap();
    assert_eq!(devices.len(), 1, "{devices:?}");
    let d = &devices[0];
    assert_eq!(d.bus, Bus::Usb);
    assert_eq!(d.size, 1 << 30);
    assert_eq!((d.logical, d.physical), (512, 4096));
    assert_eq!(d.model, "Generic STORAGE DEVICE");
    assert_eq!(d.diskseq, Some(17));
    assert!(d.removable);
    assert_eq!(d.partitions.len(), 2);
    assert_eq!(d.partitions[1].offset, 512 << 20);
    assert_eq!(d.partitions[1].label, "iOS");
    assert_eq!(d.partitions[0].device, (8, 17));
}

#[test]
fn sd_card_slot_counts_as_removable_and_emmc_does_not() {
    let tmp = tempfile::tempdir().unwrap();
    let sys = tmp.path();
    let sd = device(
        sys,
        "platform/sdhci/mmc_host/mmc0/mmc0:aaaa",
        "mmcblk0",
        "179:0",
        1 << 20,
        false,
    );
    write(&sd.join("device/type"), "SD\n");
    let emmc = device(
        sys,
        "platform/sdhci/mmc_host/mmc1/mmc1:0001",
        "mmcblk1",
        "179:8",
        1 << 20,
        false,
    );
    write(&emmc.join("device/type"), "MMC\n");
    let devices = read_block_devices(sys).unwrap();
    assert_eq!(devices[0].bus, Bus::Sd);
    assert!(devices[0].removable);
    assert_eq!(devices[1].bus, Bus::Other("eMMC".into()));
    assert!(!devices[1].removable);
}

#[test]
fn underlying_follows_slaves_down() {
    let tmp = tempfile::tempdir().unwrap();
    let sys = tmp.path();
    write(&sys.join("dev/block/253:1/slaves/dm-0/dev"), "253:0\n");
    write(&sys.join("dev/block/253:0/slaves/nvme0n1p3/dev"), "259:3\n");
    fs::create_dir_all(sys.join("dev/block/259:3")).unwrap();
    assert_eq!(underlying(sys, (253, 1)), vec![(259, 3)]);
    assert_eq!(underlying(sys, (8, 1)), vec![(8, 1)]);
}

#[test]
fn bus_from_path() {
    assert_eq!(
        bus_of(
            "/sys/devices/pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0/block/sda",
            "sda",
            ""
        ),
        Bus::Sata
    );
    assert_eq!(bus_of("/sys/devices/virtual/block/loop0", "loop0", ""), Bus::Virtual);
    assert_eq!(
        bus_of(
            "/sys/devices/pci0000:00/0000:00:1d.0/0000:3d:00.0/nvme/nvme0/nvme0n1",
            "nvme0n1",
            ""
        ),
        Bus::Nvme
    );
    assert_eq!(parse_dev("8:16\n"), Some((8, 16)));
}
