// SPDX-License-Identifier: GPL-3.0-or-later
//! The Data seed's DBRDISK1 geometry (native_xnu_layout.py validate_data_seed): format only, not its contents.

use crate::Error;
use crate::kernel::PAGE;

const MAGIC: &[u8; 8] = b"DBRDISK1";
const HEADER: usize = 32;
const EXTENT: usize = 16;
const MAX_EXTENTS: u64 = 65536;

fn refused(why: &str) -> Error {
    Error::Layout(why.to_owned())
}

fn u64_at(data: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(data[at..at + 8].try_into().expect("eight bytes"))
}

pub fn check(data: &[u8]) -> Result<(), Error> {
    if !(HEADER..=16 << 20).contains(&data.len()) {
        return Err(refused("NativeDataSeed must be 32 bytes..16 MiB"));
    }
    let version = u32::from_le_bytes(data[8..12].try_into().expect("four bytes"));
    let block = u32::from_le_bytes(data[12..16].try_into().expect("four bytes"));
    if &data[..8] != MAGIC || version != 1 || u64::from(block) != PAGE {
        return Err(refused("NativeDataSeed requires DBRDISK1 version 1 with 4 KiB pages"));
    }
    let (logical, count) = (u64_at(data, 16), u64_at(data, 24));
    let len = data.len() as u64;
    if logical == 0 || logical > 1 << 30 || logical % PAGE != 0 || count > MAX_EXTENTS || count > (len - 32) / 16 {
        return Err(refused("NativeDataSeed logical size or extent count is invalid"));
    }
    let (mut cursor, mut previous_end) = (HEADER as u64 + count * EXTENT as u64, 0);
    for index in 0..count as usize {
        let (at, size) = (
            u64_at(data, HEADER + index * EXTENT),
            u64_at(data, HEADER + index * EXTENT + 8),
        );
        if size == 0
            || (at | size) % PAGE != 0
            || at < previous_end
            || at > logical
            || size > logical - at
            || size > len - cursor
        {
            return Err(refused(
                "NativeDataSeed extent is unaligned, overlapping or out of bounds",
            ));
        }
        previous_end = at + size;
        cursor += size;
    }
    if cursor != len {
        return Err(refused("NativeDataSeed has trailing bytes"));
    }
    Ok(())
}
