// SPDX-License-Identifier: GPL-3.0-or-later
//! The commit's write order and what it refuses from a `Fill`, on hand-made plans; and how a reinstall finds the
//! descriptor areas of the container a partition held before.

use std::io;
use std::ops::Range;
use std::sync::Arc;

use switch_apfs_alloc::Bitmap;
use switch_apfs_format::container::{NX_MAGIC, NxSuperblock, XP_NONCONTIGUOUS};
use switch_apfs_format::io::BlockTarget;
use switch_apfs_format::object::{OBJ_PHYSICAL, ObjPhys, TYPE_NX_SUPERBLOCK};
use switch_apfs_format::{BLOCK_SIZE, put, seal};
use zerocopy::FromZeros;
use zerocopy::little_endian::{U32, U64};

use crate::checkpoint::Committed;
use crate::commit::{Phase, write_until};
use crate::content::{Fill, Sequential};
use crate::place::{NewContent, NewData};
use crate::plan::Plan;
use crate::target::{MemTarget, stale_descriptor_area};

fn bare_plan(invalidate: Vec<Range<u64>>, new_data: Vec<NewData>) -> Plan {
    let block = || Box::new([0u8; BLOCK_SIZE]);
    Plan {
        target_blocks: 100,
        xid: 2,
        desc_area: 10..12,
        data_area: 12..20,
        invalidate,
        copy: Bitmap::new(100),
        relocations: Vec::new(),
        new_data,
        metadata: Vec::new(),
        checkpoint: Committed {
            ephemeral: Vec::new(),
            map: (10, block()),
            superblock: block(),
            superblock_paddr: 11,
        },
        summary: Default::default(),
    }
}

#[derive(Default)]
struct Log(Vec<(&'static str, u64)>);

impl BlockTarget for Log {
    fn write_blocks(&mut self, paddr: u64, _: &[u8]) -> io::Result<()> {
        self.0.push(("write", paddr));
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.0.push(("flush", 0));
        Ok(())
    }
}

#[test]
fn block_zero_reaches_the_medium_before_the_areas_are_zeroed() {
    let plan = bare_plan(vec![0..1, 10..12, 50..58], Vec::new());
    let mut log = Log::default();
    write_until(&MemTarget::default(), &mut log, &plan, Phase::Invalidate).unwrap();
    assert_eq!(log.0[..2], [("write", 0), ("flush", 0)], "phase B wrote {:?}", log.0);
    assert_eq!(log.0.iter().filter(|(w, _)| *w == "write").count(), 3);
}

fn fill_plan(fill: Arc<dyn Fill>) -> Plan {
    let data = NewData {
        dst: 30,
        blocks: 3,
        content: NewContent::Fill(fill),
    };
    bare_plan(Vec::new(), vec![data])
}

fn stale_card() -> MemTarget {
    let mut mem = MemTarget::default();
    mem.write_blocks(29, &vec![0xAAu8; 5 * BLOCK_SIZE]).unwrap();
    mem
}

fn produce(n: usize) -> impl Fn(&mut dyn FnMut(&[u8]) -> io::Result<()>) -> io::Result<()> + Send + Sync {
    move |out: &mut dyn FnMut(&[u8]) -> io::Result<()>| out(&vec![1u8; n])
}

#[test]
fn a_fill_that_stops_short_inside_its_last_block_is_refused() {
    let short = Sequential::new(3 * BLOCK_SIZE as u64 - 5, produce(3 * BLOCK_SIZE - 10));
    let r = write_until(
        &MemTarget::default(),
        &mut stale_card(),
        &fill_plan(Arc::new(short)),
        Phase::Data,
    );
    assert!(
        r.is_err(),
        "a fill 5 bytes short of its stream, inside its last block, was accepted"
    );
}

#[test]
fn a_fill_that_produces_more_than_its_stream_is_refused() {
    let long = Sequential::new(2 * BLOCK_SIZE as u64, produce(2 * BLOCK_SIZE + 1));
    let r = write_until(
        &MemTarget::default(),
        &mut stale_card(),
        &fill_plan(Arc::new(long)),
        Phase::Data,
    );
    assert!(r.is_err(), "a fill one byte longer than its stream was accepted");
}

#[test]
fn a_fill_that_leaves_blocks_of_its_extent_unwritten_is_refused() {
    let short = Sequential::new(10, |out: &mut dyn FnMut(&[u8]) -> io::Result<()>| out(&[1u8; 10]));
    let mut mem = stale_card();
    let r = write_until(
        &MemTarget::default(),
        &mut mem,
        &fill_plan(Arc::new(short)),
        Phase::Data,
    );
    assert!(
        r.is_err(),
        "a fill that wrote 1 of 3 blocks was accepted; block 31 still holds {:#x}",
        mem.blocks[&31][0]
    );

    struct Twice;
    impl Fill for Twice {
        fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
            window.write_blocks(0, &[1u8; BLOCK_SIZE])?;
            window.write_blocks(0, &[1u8; BLOCK_SIZE])?;
            window.write_blocks(2, &[1u8; BLOCK_SIZE])
        }
    }
    let r = write_until(
        &MemTarget::default(),
        &mut stale_card(),
        &fill_plan(Arc::new(Twice)),
        Phase::Data,
    );
    assert!(r.is_err(), "three writes that miss block 1 were accepted");
}

#[test]
fn a_fill_writing_far_past_its_window_is_refused_without_wrapping() {
    struct Wild;
    impl Fill for Wild {
        fn fill(&self, window: &mut dyn BlockTarget) -> io::Result<()> {
            window.write_blocks(u64::MAX, &[7u8; BLOCK_SIZE])
        }
    }
    let mut mem = stale_card();
    let r = write_until(&MemTarget::default(), &mut mem, &fill_plan(Arc::new(Wild)), Phase::Data);
    assert!(r.is_err());
    assert_eq!(mem.blocks[&29][0], 0xAA, "the write landed before the extent");
}

fn nxsb(xid: u64, desc: Range<u64>, flags: u32) -> Vec<u8> {
    let mut nx = NxSuperblock::new_zeroed();
    nx.o = ObjPhys::new(1, xid, OBJ_PHYSICAL | TYPE_NX_SUPERBLOCK, 0);
    nx.magic = U32::new(NX_MAGIC);
    nx.block_size = U32::new(BLOCK_SIZE as u32);
    nx.block_count = U64::new(1000);
    nx.xp_desc_base = U64::new(desc.start);
    nx.xp_desc_blocks = U32::new((desc.end - desc.start) as u32 | flags);
    let mut block = vec![0u8; BLOCK_SIZE];
    put(&mut block, 0, &nx);
    seal(&mut block);
    block
}

fn areas(mem: &MemTarget) -> Vec<Range<u64>> {
    stale_descriptor_area(mem).unwrap().into_iter().collect()
}

#[test]
fn a_lagging_block_zero_still_leads_to_the_newest_area() {
    let mut mem = MemTarget::default();
    mem.write_blocks(0, &nxsb(5, 100..108, 0)).unwrap();
    mem.write_blocks(100, &nxsb(5, 100..108, 0)).unwrap();
    mem.write_blocks(101, &nxsb(6, 200..210, 0)).unwrap();
    mem.write_blocks(201, &nxsb(7, 200..210, 0)).unwrap();
    assert_eq!(areas(&mem), [100..108, 200..210]);

    let mut current = MemTarget::default();
    current.write_blocks(0, &nxsb(7, 200..210, 0)).unwrap();
    current.write_blocks(201, &nxsb(7, 200..210, 0)).unwrap();
    let once = areas(&current);
    assert_eq!((once.len(), &once[0]), (1, &(200..210)), "the area block 0 names, once");
    assert!(areas(&MemTarget::default()).is_empty());
}

#[test]
fn a_noncontiguous_descriptor_area_is_refused() {
    let mut mem = MemTarget::default();
    mem.write_blocks(0, &nxsb(5, 100..108, XP_NONCONTIGUOUS)).unwrap();
    assert!(stale_descriptor_area(&mem).is_err());
}
