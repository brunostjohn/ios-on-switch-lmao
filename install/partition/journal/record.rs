// SPDX-License-Identifier: GPL-3.0-or-later
//! What one journaled step changes: groups of records, each group written and made durable before the next. Every
//! record is an absolute value with the one it replaces, so applying a step again after a cut is a no-op wherever it
//! landed and a refusal wherever something else changed the card.

use crate::error::{Error, Result};

/// How a `Field` is stored at its offset.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldKind {
    Le8,
    Le16,
    Le32,
    Le64,
    /// A FAT32 directory entry's first cluster: its high half at +20, its low half at +26.
    Fat32Cluster,
    /// An exFAT file entry (0x85): `offset` is the entry base, updates `SetChecksum` at +2.
    ExfatChecksum,
    /// An exFAT stream extension (0xC0): `offset` is the entry base, updates `GeneralSecondaryFlags` at +1.
    ExfatStreamFlags,
    /// An exFAT stream extension (0xC0): `offset` is the entry base, updates `FirstCluster` at +20.
    ExfatStreamCluster,
    /// An exFAT allocation bitmap entry (0x81): `offset` is the entry base, updates `DataLength` at +24.
    ExfatBitmapLen,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Record {
    /// FAT entries `first..first+len` chained in order, the last one set to `last`. They held 0 before, or anything
    /// when `loose` (exFAT: entries of free or contiguous clusters mean nothing).
    Chain {
        first: u32,
        len: u32,
        last: u32,
        loose: bool,
    },
    /// FAT entries `first..first+len` freed; before, they chained in order to `last`.
    Free {
        first: u32,
        len: u32,
        last: u32,
    },
    Fat {
        index: u32,
        old: u32,
        new: u32,
    },
    Field {
        lba: u64,
        offset: u16,
        kind: FieldKind,
        old: u64,
        new: u64,
    },
    /// A FAT32 directory's first cluster moved from `old` to `new`: its own `.` and every subdirectory's `..` follow.
    /// For the root (`root`), there is no `.` and a `..` of 0 stays.
    DotDot {
        old: u32,
        new: u32,
        root: bool,
    },
    /// `count` sectors from `lba` cleared to zeros.
    Zero {
        lba: u64,
        count: u32,
    },
    /// A whole sector: before, contents with CRC-32 `old`.
    Sector {
        lba: u64,
        old: u32,
        new: Box<[u8; 512]>,
    },
    /// exFAT allocation bitmap bits for clusters `first..first+len`, set (`true`) or cleared (`false`).
    Bits {
        first: u32,
        len: u32,
        set: bool,
    },
}

/// One step: its groups, in order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Step {
    pub groups: Vec<Vec<Record>>,
}

const END_OF_GROUP: u8 = 0xfe;

impl Record {
    pub fn encoded_len(&self) -> usize {
        1 + match self {
            Record::Chain { .. } => 13,
            Record::Free { .. } | Record::Fat { .. } | Record::Zero { .. } => 12,
            Record::Field { .. } => 27,
            Record::DotDot { .. } | Record::Bits { .. } => 9,
            Record::Sector { .. } => 12 + 512,
        }
    }

    fn encode(&self, out: &mut Vec<u8>) {
        let u32s = |out: &mut Vec<u8>, tag: u8, values: &[u32]| {
            out.push(tag);
            for v in values {
                out.extend_from_slice(&v.to_le_bytes());
            }
        };
        match self {
            Record::Chain {
                first,
                len,
                last,
                loose,
            } => {
                u32s(out, 1, &[*first, *len, *last]);
                out.push(u8::from(*loose));
            }
            Record::Free { first, len, last } => u32s(out, 2, &[*first, *len, *last]),
            Record::Fat { index, old, new } => u32s(out, 3, &[*index, *old, *new]),
            Record::Field {
                lba,
                offset,
                kind,
                old,
                new,
            } => {
                out.push(4);
                out.extend_from_slice(&lba.to_le_bytes());
                out.extend_from_slice(&offset.to_le_bytes());
                out.push(kind.code());
                out.extend_from_slice(&old.to_le_bytes());
                out.extend_from_slice(&new.to_le_bytes());
            }
            Record::DotDot { old, new, root } => {
                u32s(out, 5, &[*old, *new]);
                out.push(u8::from(*root));
            }
            Record::Zero { lba, count } => {
                out.push(6);
                out.extend_from_slice(&lba.to_le_bytes());
                out.extend_from_slice(&count.to_le_bytes());
            }
            Record::Sector { lba, old, new } => {
                out.push(7);
                out.extend_from_slice(&lba.to_le_bytes());
                out.extend_from_slice(&old.to_le_bytes());
                out.extend_from_slice(&new[..]);
            }
            Record::Bits { first, len, set } => {
                u32s(out, 8, &[*first, *len]);
                out.push(u8::from(*set));
            }
        }
    }
}

impl FieldKind {
    fn code(self) -> u8 {
        match self {
            FieldKind::Le8 => 1,
            FieldKind::Le16 => 2,
            FieldKind::Le32 => 4,
            FieldKind::Le64 => 8,
            FieldKind::Fat32Cluster => 0x20,
            FieldKind::ExfatChecksum => 0x21,
            FieldKind::ExfatStreamFlags => 0x22,
            FieldKind::ExfatStreamCluster => 0x23,
            FieldKind::ExfatBitmapLen => 0x24,
        }
    }

    fn from_code(code: u8) -> Option<FieldKind> {
        Some(match code {
            1 => FieldKind::Le8,
            2 => FieldKind::Le16,
            4 => FieldKind::Le32,
            8 => FieldKind::Le64,
            0x20 => FieldKind::Fat32Cluster,
            0x21 => FieldKind::ExfatChecksum,
            0x22 => FieldKind::ExfatStreamFlags,
            0x23 => FieldKind::ExfatStreamCluster,
            0x24 => FieldKind::ExfatBitmapLen,
            _ => return None,
        })
    }

    pub fn valid_offset(self, offset: u16) -> bool {
        let at = offset as usize;
        match self {
            FieldKind::Le8 => at < 512,
            FieldKind::Le16 => at + 2 <= 512,
            FieldKind::Le32 => at + 4 <= 512,
            FieldKind::Le64 => at + 8 <= 512,
            FieldKind::Fat32Cluster
            | FieldKind::ExfatChecksum
            | FieldKind::ExfatStreamFlags
            | FieldKind::ExfatStreamCluster
            | FieldKind::ExfatBitmapLen => at.is_multiple_of(32) && at + 32 <= 512,
        }
    }
}

impl Step {
    pub fn encoded_len(&self) -> usize {
        self.groups
            .iter()
            .map(|g| 1 + g.iter().map(Record::encoded_len).sum::<usize>())
            .sum()
    }

    pub fn encode(&self, out: &mut Vec<u8>) {
        for group in &self.groups {
            for r in group {
                r.encode(out);
            }
            out.push(END_OF_GROUP);
        }
    }

    pub fn decode(mut bytes: &[u8]) -> Result<Step> {
        let bad = || Error::Journal("a committed step does not decode");
        let mut step = Step::default();
        let mut group = Vec::new();
        while let Some((&tag, rest)) = bytes.split_first() {
            bytes = rest;
            let mut take = |n: usize| -> Result<&[u8]> {
                let (head, rest) = bytes.split_at_checked(n).ok_or_else(bad)?;
                bytes = rest;
                Ok(head)
            };
            let u32_at = |b: &[u8], i: usize| u32::from_le_bytes(b[4 * i..4 * i + 4].try_into().unwrap());
            let record = match tag {
                END_OF_GROUP => {
                    step.groups.push(std::mem::take(&mut group));
                    continue;
                }
                1 => {
                    let b = take(13)?;
                    Record::Chain {
                        first: u32_at(b, 0),
                        len: u32_at(b, 1),
                        last: u32_at(b, 2),
                        loose: b[12] != 0,
                    }
                }
                2 | 3 => {
                    let b = take(12)?;
                    let (x, y, z) = (u32_at(b, 0), u32_at(b, 1), u32_at(b, 2));
                    if tag == 2 {
                        Record::Free {
                            first: x,
                            len: y,
                            last: z,
                        }
                    } else {
                        Record::Fat {
                            index: x,
                            old: y,
                            new: z,
                        }
                    }
                }
                4 => {
                    let b = take(27)?;
                    let offset = u16::from_le_bytes([b[8], b[9]]);
                    let kind = FieldKind::from_code(b[10]).ok_or_else(bad)?;
                    if !kind.valid_offset(offset) {
                        return Err(bad());
                    }
                    Record::Field {
                        lba: u64::from_le_bytes(b[..8].try_into().unwrap()),
                        offset,
                        kind,
                        old: u64::from_le_bytes(b[11..19].try_into().unwrap()),
                        new: u64::from_le_bytes(b[19..27].try_into().unwrap()),
                    }
                }
                5 => {
                    let b = take(9)?;
                    Record::DotDot {
                        old: u32_at(b, 0),
                        new: u32_at(b, 1),
                        root: b[8] != 0,
                    }
                }
                6 => {
                    let b = take(12)?;
                    Record::Zero {
                        lba: u64::from_le_bytes(b[..8].try_into().unwrap()),
                        count: u32_at(b, 2),
                    }
                }
                7 => {
                    let b = take(524)?;
                    Record::Sector {
                        lba: u64::from_le_bytes(b[..8].try_into().unwrap()),
                        old: u32_at(b, 2),
                        new: Box::new(b[12..].try_into().unwrap()),
                    }
                }
                8 => {
                    let b = take(9)?;
                    Record::Bits {
                        first: u32_at(b, 0),
                        len: u32_at(b, 1),
                        set: b[8] != 0,
                    }
                }
                _ => return Err(bad()),
            };
            group.push(record);
        }
        if !group.is_empty() {
            return Err(bad());
        }
        Ok(step)
    }
}
