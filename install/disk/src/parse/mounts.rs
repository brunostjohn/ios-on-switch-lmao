// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mount {
    pub device: (u32, u32),
    pub mount_point: PathBuf,
    pub file_system: String,
    pub source: String,
}

/// `/proc/self/mountinfo` (proc(5)). Lines that don't parse are skipped.
pub fn parse_mountinfo(text: &str) -> Vec<Mount> {
    text.lines().filter_map(parse_mountinfo_line).collect()
}

fn parse_mountinfo_line(line: &str) -> Option<Mount> {
    let (front, back) = line.split_once(" - ")?;
    let mut fields = front.split(' ');
    let device = fields.nth(2)?;
    let mount_point = fields.nth(1)?;
    let (major, minor) = device.split_once(':')?;
    let mut back = back.split(' ');
    let file_system = back.next()?;
    let source = back.next().unwrap_or_default();
    Some(Mount {
        device: (major.parse().ok()?, minor.parse().ok()?),
        mount_point: PathBuf::from(unescape(mount_point)),
        file_system: file_system.to_owned(),
        source: unescape(source),
    })
}

/// The device paths in `/proc/swaps`, after its header line.
pub fn parse_swaps(text: &str) -> Vec<PathBuf> {
    text.lines()
        .skip(1)
        .filter_map(|line| line.split_whitespace().next())
        .map(|path| PathBuf::from(unescape(path)))
        .collect()
}

/// The kernel writes space, tab, newline and backslash in paths as three-digit octal escapes.
fn unescape(field: &str) -> String {
    let bytes = field.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'\\' && i + 3 < bytes.len() && bytes[i + 1..i + 4].iter().all(|b| (b'0'..=b'7').contains(b)) {
            let code = bytes[i + 1..i + 4]
                .iter()
                .fold(0u32, |acc, b| acc * 8 + u32::from(b - b'0'));
            out.push(code as u8);
            i += 4;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mountinfo_lines() {
        let text = "\
22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw
95 22 8:17 / /media/me/My\\040Card rw,nosuid shared:50 - vfat /dev/sdb1 rw,uid=1000
96 22 8:18 / /media/me/x rw master:1 shared:2 - apfs /dev/sdb2 rw
garbage
";
        let mounts = parse_mountinfo(text);
        assert_eq!(mounts.len(), 3);
        assert_eq!(mounts[0].device, (259, 2));
        assert_eq!(mounts[0].mount_point, PathBuf::from("/"));
        assert_eq!(mounts[1].mount_point, PathBuf::from("/media/me/My Card"));
        assert_eq!(mounts[1].file_system, "vfat");
        assert_eq!(mounts[1].source, "/dev/sdb1");
        assert_eq!(mounts[2].device, (8, 18));
    }

    #[test]
    fn swaps() {
        let text = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/dev/sda3 partition 8388604 0 -2\n";
        assert_eq!(parse_swaps(text), vec![PathBuf::from("/dev/sda3")]);
    }

    #[test]
    fn escapes() {
        assert_eq!(unescape("a\\040b\\134c"), "a b\\c");
        assert_eq!(unescape("trailing\\04"), "trailing\\04");
    }
}
