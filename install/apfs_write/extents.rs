// SPDX-License-Identifier: GPL-3.0-or-later
//! The extent-reference tree's records. Every allocated run is split wherever an extent starts or ends, and each
//! piece counts the extents that cover it: a rewritten clone's shared blocks get a record with refcount 2 and its own
//! blocks one with refcount 1, as macOS's kext splits them.

use std::collections::BTreeMap;

use switch_apfs_format::fs::{APFS_KIND_NEW, APFS_TYPE_EXTENT, JKey, PEXT_KIND_SHIFT, PhysExtVal};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{I32, U64};

use crate::records::Record;

/// A run of blocks one stream's extent uses, and the object the kext would name as its owner.
#[derive(Clone, Copy, Debug)]
pub struct Owned {
    pub paddr: u64,
    pub blocks: u64,
    pub owner: u64,
}

pub struct ExtentRefs {
    pub records: Vec<Record>,
    /// Each block counted once, however many extents share it.
    pub blocks: u64,
    pub shared_blocks: u64,
}

pub fn build(owned: &[Owned]) -> ExtentRefs {
    let mut events: Vec<(u64, bool, u64)> = Vec::with_capacity(owned.len() * 2);
    for o in owned.iter().filter(|o| o.blocks > 0) {
        events.push((o.paddr, true, o.owner));
        events.push((o.paddr + o.blocks, false, o.owner));
    }
    events.sort_unstable();
    let mut active: BTreeMap<u64, u32> = BTreeMap::new();
    let mut covering = 0u32;
    let mut out = ExtentRefs {
        records: Vec::new(),
        blocks: 0,
        shared_blocks: 0,
    };
    let mut i = 0;
    while i < events.len() {
        let at = events[i].0;
        while i < events.len() && events[i].0 == at {
            let (_, start, owner) = events[i];
            if start {
                *active.entry(owner).or_default() += 1;
                covering += 1;
            } else {
                if let Some(n) = active.get_mut(&owner) {
                    *n -= 1;
                    if *n == 0 {
                        active.remove(&owner);
                    }
                }
                covering -= 1;
            }
            i += 1;
        }
        let Some(&(next, _, _)) = events.get(i) else {
            break;
        };
        let Some((&owner, _)) = active.first_key_value() else {
            continue;
        };
        let blocks = next - at;
        let value = PhysExtVal {
            len_and_kind: U64::new(blocks | APFS_KIND_NEW << PEXT_KIND_SHIFT),
            owning_obj_id: U64::new(owner),
            refcnt: I32::new(covering as i32),
        };
        out.records.push((
            JKey::new(at, APFS_TYPE_EXTENT).as_bytes().to_vec(),
            Some(value.as_bytes().to_vec()),
        ));
        out.blocks += blocks;
        if covering > 1 {
            out.shared_blocks += blocks;
        }
    }
    out
}
