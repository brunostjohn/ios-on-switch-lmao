// SPDX-License-Identifier: GPL-3.0-or-later
//! The Windows storage IOCTL replies, read from their bytes (winioctl.h layouts, 64-bit). The Windows module asserts
//! these offsets against windows-sys's own structs at compile time.
use crate::model::Bus;

pub const DEVICE_DESCRIPTOR_REMOVABLE: usize = 10;
pub const DEVICE_DESCRIPTOR_VENDOR: usize = 12;
pub const DEVICE_DESCRIPTOR_PRODUCT: usize = 16;
pub const DEVICE_DESCRIPTOR_BUS: usize = 28;
pub const ALIGNMENT_LOGICAL: usize = 16;
pub const ALIGNMENT_PHYSICAL: usize = 20;
pub const LAYOUT_HEADER: usize = 48;
pub const LAYOUT_ENTRY: usize = 144;
pub const EXTENTS_HEADER: usize = 8;
pub const GEOMETRY_SECTOR: usize = 20;
pub const GEOMETRY_SIZE: usize = 24;
pub const EXTENT: usize = 24;

const STYLE_MBR: u32 = 0;
const STYLE_GPT: u32 = 1;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceDescriptor {
    pub removable: bool,
    pub bus: Bus,
    pub model: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LayoutEntry {
    pub number: u32,
    pub offset: u64,
    pub size: u64,
    pub kind: String,
    pub name: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Extent {
    pub disk: u32,
    pub offset: u64,
    pub length: u64,
}

/// STORAGE_DEVICE_DESCRIPTOR.
pub fn device_descriptor(bytes: &[u8]) -> Option<DeviceDescriptor> {
    let vendor = c_string_at(bytes, u32_at(bytes, DEVICE_DESCRIPTOR_VENDOR)? as usize);
    let product = c_string_at(bytes, u32_at(bytes, DEVICE_DESCRIPTOR_PRODUCT)? as usize);
    let model = [vendor, product]
        .into_iter()
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>()
        .join(" ");
    Some(DeviceDescriptor {
        removable: *bytes.get(DEVICE_DESCRIPTOR_REMOVABLE)? != 0,
        bus: bus_type(u32_at(bytes, DEVICE_DESCRIPTOR_BUS)?),
        model,
    })
}

/// STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR: (logical, physical) sector sizes.
pub fn alignment(bytes: &[u8]) -> Option<(u32, u32)> {
    Some((u32_at(bytes, ALIGNMENT_LOGICAL)?, u32_at(bytes, ALIGNMENT_PHYSICAL)?))
}

/// DISK_GEOMETRY_EX: (disk size, bytes per sector).
pub fn geometry(bytes: &[u8]) -> Option<(u64, u32)> {
    Some((u64_at(bytes, GEOMETRY_SIZE)?, u32_at(bytes, GEOMETRY_SECTOR)?))
}

/// DRIVE_LAYOUT_INFORMATION_EX. Unused MBR slots and entries without a number are left out.
pub fn drive_layout(bytes: &[u8]) -> Option<Vec<LayoutEntry>> {
    let count = u32_at(bytes, 4)? as usize;
    let mut entries = Vec::new();
    for i in 0..count {
        let at = LAYOUT_HEADER + i * LAYOUT_ENTRY;
        let entry = bytes.get(at..at + LAYOUT_ENTRY)?;
        let number = u32_at(entry, 24)?;
        let (kind, name) = match u32_at(entry, 0)? {
            STYLE_MBR if entry[32] != 0 => (format!("{:02x}", entry[32]), String::new()),
            STYLE_GPT => (guid(&entry[32..48]), utf16_name(&entry[72..144])),
            _ => continue,
        };
        if number == 0 {
            continue;
        }
        entries.push(LayoutEntry {
            number,
            offset: u64_at(entry, 8)?,
            size: u64_at(entry, 16)?,
            kind,
            name,
        });
    }
    Some(entries)
}

/// VOLUME_DISK_EXTENTS.
pub fn disk_extents(bytes: &[u8]) -> Option<Vec<Extent>> {
    let count = u32_at(bytes, 0)? as usize;
    (0..count)
        .map(|i| {
            let at = EXTENTS_HEADER + i * EXTENT;
            Some(Extent {
                disk: u32_at(bytes, at)?,
                offset: u64_at(bytes, at + 8)?,
                length: u64_at(bytes, at + 16)?,
            })
        })
        .collect()
}

/// STORAGE_BUS_TYPE.
pub fn bus_type(value: u32) -> Bus {
    match value {
        1 | 5 | 6 | 9 | 0xa => Bus::Scsi,
        2 | 3 | 0xb => Bus::Sata,
        7 => Bus::Usb,
        0xc | 0xd => Bus::Sd,
        0xe | 0xf => Bus::Virtual,
        0x11 => Bus::Nvme,
        other => Bus::Other(format!("bus type {other}")),
    }
}

/// A GUID as text, its first three fields little-endian as Windows stores them.
pub fn guid(b: &[u8]) -> String {
    format!(
        "{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
        u16::from_le_bytes([b[4], b[5]]),
        u16::from_le_bytes([b[6], b[7]]),
        b[8],
        b[9],
        b[10],
        b[11],
        b[12],
        b[13],
        b[14],
        b[15]
    )
}

fn utf16_name(bytes: &[u8]) -> String {
    let units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .take_while(|&u| u != 0)
        .collect();
    String::from_utf16_lossy(&units)
}

fn c_string_at(bytes: &[u8], at: usize) -> String {
    if at == 0 || at >= bytes.len() {
        return String::new();
    }
    let end = bytes[at..].iter().position(|&b| b == 0).map_or(bytes.len(), |n| at + n);
    String::from_utf8_lossy(&bytes[at..end]).trim().to_owned()
}

fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
}

fn u64_at(bytes: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(bytes.get(at..at + 8)?.try_into().ok()?))
}
