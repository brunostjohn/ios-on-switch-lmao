// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::{self, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

mod cases;

use cases::{cases, expected, listed};
use switch_ipsw::{Container, member_container, os_container, sniff, udif_disk};

/// Compares what is written with a file, byte for byte, and remembers where they first differ.
struct Compare {
    file: BufReader<File>,
    pos: u64,
    first_difference: Option<u64>,
    buf: Vec<u8>,
}

impl Compare {
    fn new(path: &Path, from: u64) -> Compare {
        let mut file = File::open(path).unwrap();
        file.seek(SeekFrom::Start(from)).unwrap();
        Compare {
            file: BufReader::with_capacity(8 << 20, file),
            pos: from,
            first_difference: None,
            buf: Vec::new(),
        }
    }
}

impl Write for Compare {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buf.resize(data.len(), 0);
        let mut got = 0;
        while got < data.len() {
            match self.file.read(&mut self.buf[got..])? {
                0 => break,
                n => got += n,
            }
        }
        if self.first_difference.is_none() && (got < data.len() || self.buf[..] != *data) {
            let at = (0..got).find(|&i| self.buf[i] != data[i]).unwrap_or(got);
            self.first_difference = Some(self.pos + at as u64);
        }
        self.pos += data.len() as u64;
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn apfs_slice(image: &Path) -> (u64, u64) {
    let dmg = udif::DmgReader::with_options(
        File::open(image).unwrap(),
        udif::DmgReaderOptions {
            verify_checksums: false,
        },
    );
    let dmg = dmg.unwrap();
    let apfs = dmg.partitions().iter().find(|p| p.name.contains("Apple_APFS")).unwrap();
    (apfs.block_map.first_sector * 512, apfs.block_map.sector_count * 512)
}

/// Every stored, unencrypted member that is a UDIF image (21H461's OS today), unwrapped by `udif` and by
/// `hdiutil convert -format UDTO`, compared byte for byte: the whole disk, then the APFS container alone, which must
/// also be the one the catalog pins.
#[test]
fn udif_images_match_hdiutil() {
    let cases = cases("SWITCH_STORED_IMAGES");
    assert_eq!(
        listed(&cases),
        expected(&cases::catalog(), |m| !m.deflated && m.aea.is_none())
    );
    let tmp = PathBuf::from(std::env::var("TEST_TMPDIR").unwrap());
    let mut udif_images = 0;
    for case in cases {
        let image = &case.files[0];
        if sniff(&mut File::open(image).unwrap()).unwrap() != Some(Container::Udif) {
            continue;
        }
        udif_images += 1;
        let name = format!("{} {}", case.kind, case.role);
        let raw = tmp.join(format!("{}-{}.cdr", case.kind, case.role));
        let started = Instant::now();
        let status = Command::new("/usr/bin/hdiutil")
            .args(["convert", "-quiet", "-format", "UDTO", "-o"])
            .arg(&raw)
            .arg(image)
            .status()
            .unwrap();
        assert!(status.success(), "{name}: hdiutil {status}");
        let hdiutil_seconds = started.elapsed().as_secs_f64();
        let raw_len = std::fs::metadata(&raw).unwrap().len();

        let started = Instant::now();
        let mut disk = Compare::new(&raw, 0);
        let written = udif_disk(File::open(image).unwrap(), &mut disk).unwrap();
        let udif_seconds = started.elapsed().as_secs_f64();
        let (offset, len) = apfs_slice(image);
        let mut apfs = Compare::new(&raw, offset);
        let container = os_container(File::open(image).unwrap(), &mut apfs, Container::Udif).unwrap();
        std::fs::remove_file(&raw).unwrap();

        println!(
            "{name}: hdiutil {raw_len} B in {hdiutil_seconds:.1} s; udif {} B in {udif_seconds:.1} s, sha256 {}; \
             APFS at {offset}+{len}, sha256 {}",
            written.len,
            hex::encode(written.sha256),
            hex::encode(container.sha256),
        );
        assert_eq!(disk.first_difference, None, "{name}: the disk differs from hdiutil's");
        assert_eq!(written.len, raw_len, "{name}: disk length");
        assert_eq!(
            apfs.first_difference, None,
            "{name}: the container differs from hdiutil's"
        );
        assert_eq!(container.len, len, "{name}: container length");
        let pinned = member_container(File::open(image).unwrap(), &case.member, &mut io::sink()).unwrap();
        assert_eq!(pinned, container, "{name}: the catalog's container pins");
        println!(
            "{name}: the container is the catalog's, {} B, sha256 {}",
            pinned.len,
            hex::encode(pinned.sha256)
        );
    }
    assert!(udif_images > 0, "no pinned member is a UDIF image");
}
