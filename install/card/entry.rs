// SPDX-License-Identifier: GPL-3.0-or-later
//! One GPT entry's first 128 bytes, as the kernel's rules read them (`SwitchSdRoot.h`, `partition.c`).

pub const ENTRY_BYTES: usize = 128;
const NAME_UNITS: usize = 36;

pub const APFS_TYPE: [u8; 16] = [
    0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
];
pub const LINUX_TYPE: [u8; 16] = [
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
];
pub const BASIC_DATA_TYPE: [u8; 16] = [
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
];
pub const EMUMMC_TYPE: [u8; 16] = [
    0x00, 0x7e, 0xca, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x65, 0x6d, 0x75, 0x4d, 0x4d, 0x43,
];

/// The kernel's `kPartitionName`: an entry with this name is a root whatever its type.
pub const ROOT_NAME: &str = "iOS";
/// An entry being written: never a root, since it is not APFS-typed either.
pub const SETUP_NAME: &str = "iOS-setup";
pub const DEMOTED_NAME: &str = "userdata";
/// Android's userdata, L4T's root and the UDA hekate's partition manager names: what an install may take over.
pub const FRESH_NAMES: [&str; 3] = ["userdata", "l4t", "UDA"];

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Entry(pub [u8; ENTRY_BYTES]);

impl Entry {
    pub fn type_guid(&self) -> [u8; 16] {
        self.0[..16].try_into().unwrap()
    }

    pub fn unique_guid(&self) -> [u8; 16] {
        self.0[16..32].try_into().unwrap()
    }

    pub fn first(&self) -> u64 {
        u64::from_le_bytes(self.0[32..40].try_into().unwrap())
    }

    pub fn last(&self) -> u64 {
        u64::from_le_bytes(self.0[40..48].try_into().unwrap())
    }

    pub fn attributes(&self) -> u64 {
        u64::from_le_bytes(self.0[48..56].try_into().unwrap())
    }

    pub fn is_used(&self) -> bool {
        self.type_guid() != [0; 16]
    }

    /// The name field holds exactly the ASCII `name`, as `switchSdGptNamed` compares it.
    pub fn named(&self, name: &str) -> bool {
        let field = &self.0[56..];
        let bytes = name.as_bytes();
        if bytes.len() > NAME_UNITS {
            return false;
        }
        let matches = bytes
            .iter()
            .enumerate()
            .all(|(i, &c)| field[2 * i] == c && field[2 * i + 1] == 0);
        matches && (bytes.len() == NAME_UNITS || field[2 * bytes.len()..2 * bytes.len() + 2] == [0, 0])
    }

    /// The name for display: UTF-16LE up to its first NUL.
    pub fn name(&self) -> String {
        let units: Vec<u16> = self.0[56..]
            .chunks(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .take_while(|&u| u != 0)
            .collect();
        String::from_utf16_lossy(&units)
    }

    /// `type` and `name` together; the extent, GUID and attributes stay.
    pub fn retyped(&self, type_guid: [u8; 16], name: &str) -> Entry {
        assert!(name.is_ascii() && name.len() <= NAME_UNITS);
        let mut e = *self;
        e.0[..16].copy_from_slice(&type_guid);
        e.0[56..].fill(0);
        for (i, c) in name.bytes().enumerate() {
            e.0[56 + 2 * i] = c;
        }
        e
    }

    /// `switchSdFindRoot`: APFS-typed or named `iOS`, with an extent one card command can address.
    pub fn is_kernel_root(&self) -> bool {
        (self.type_guid() == APFS_TYPE || self.named(ROOT_NAME)) && self.addressable()
    }

    pub fn addressable(&self) -> bool {
        self.first() != 0 && self.last() >= self.first() && self.last() <= u64::from(u32::MAX)
    }

    pub fn sectors(&self) -> u64 {
        self.last() - self.first() + 1
    }
}

impl std::fmt::Debug for Entry {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?} {}..={}", self.name(), self.first(), self.last())
    }
}
