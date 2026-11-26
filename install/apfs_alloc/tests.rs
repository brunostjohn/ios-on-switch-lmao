// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_format::checksum_ok;
use switch_apfs_format::spaceman::SpacemanPhys;

use crate::bitmap::Bitmap;
use crate::geometry::{CheckpointSizes, Geometry, GeometryError, MAX_BLOCKS};
use crate::space::{SpaceInput, build, cib_addresses};

/// Each row's columns after the first `skip`: blocks, desc, data, max_fs, chunks, cibs, pool, pool limit, main limit.
fn measured(text: &str, skip: usize) -> Vec<[u64; 9]> {
    text.lines()
        .filter(|l| !l.starts_with('#'))
        .map(|l| {
            let v: Vec<u64> = l.split(' ').skip(skip).map(|w| w.parse().unwrap()).collect();
            v.try_into().unwrap()
        })
        .collect()
}

fn row(g: &Geometry) -> [u64; 9] {
    [
        g.blocks,
        g.checkpoint.desc_blocks.into(),
        g.checkpoint.data_blocks.into(),
        g.max_file_systems.into(),
        g.chunks,
        g.cibs,
        g.pool_blocks,
        g.pool_queue_limit.into(),
        g.main_queue_limit.into(),
    ]
}

#[test]
fn geometry_matches_newfs_apfs() {
    let rows = measured(include_str!("testdata/newfs_apfs.txt"), 0);
    assert_eq!(rows.len(), 366);
    for want in rows {
        assert_eq!(row(&Geometry::for_blocks(want[0]).unwrap()), want);
    }
}

#[test]
fn geometry_matches_resize_container() {
    let rows = measured(include_str!("testdata/resize_container.txt"), 1);
    assert_eq!(rows.len(), 37);
    for want in rows {
        assert_eq!(row(&Geometry::for_blocks(want[0]).unwrap()), want);
    }
}

/// The OS images' containers: 24B119 has 2,213,376 blocks and 21H461 1,483,776. The Mac-built images were composed
/// at 1 TiB, and the fit kept those areas.
#[test]
fn geometry_matches_apples_containers() {
    let sizes = |desc_blocks, data_blocks| CheckpointSizes {
        desc_blocks,
        data_blocks,
    };
    let g = Geometry::for_blocks(2_213_376).unwrap();
    assert_eq!((g.chunks, g.cibs, g.pool_blocks, g.max_file_systems), (68, 1, 207, 17));
    assert_eq!(g.checkpoint, sizes(68, 6_416));
    let g = Geometry::for_blocks(1_483_776).unwrap();
    assert_eq!((g.chunks, g.cibs, g.pool_blocks, g.max_file_systems), (46, 1, 141, 12));
    assert_eq!(g.checkpoint, sizes(56, 5_136));
    assert_eq!(Geometry::for_blocks(1 << 28).unwrap().checkpoint, sizes(280, 27_748));
}

#[test]
fn refuses_sizes_outside_the_measured_range() {
    assert!(Geometry::for_blocks(MAX_BLOCKS).is_ok());
    assert_eq!(
        Geometry::for_blocks(MAX_BLOCKS + 1),
        Err(GeometryError::TooLarge(MAX_BLOCKS + 1))
    );
    assert!(Geometry::for_blocks(4096).is_ok());
    assert_eq!(Geometry::for_blocks(4095), Err(GeometryError::TooSmall(4095)));
}

#[test]
fn bitmap_runs_and_bytes() {
    let mut map = Bitmap::new(200);
    map.set_range(3..70);
    map.set_range(130..131);
    assert_eq!(map.set_runs().collect::<Vec<_>>(), vec![3..70, 130..131]);
    assert_eq!(map.count_set(0..200), 68);
    assert_eq!(map.first_clear_run(3), Some(0));
    assert_eq!(map.first_clear_run(4), Some(70));
    assert_eq!(map.first_clear_run(61), Some(131));
    assert_eq!(map.first_clear_run(70), None);
    let mut bytes = [0u8; 32];
    map.to_bytes(64..200, &mut bytes);
    assert_eq!(bytes[0], 0x3f);
    let mut back = Bitmap::new(200);
    back.set_from_bytes(64, 136, &bytes);
    assert_eq!(back.set_runs().collect::<Vec<_>>(), vec![64..70, 130..131]);
    map.clear_range(0..200);
    assert!(map.all_clear(0..200));
}

#[test]
fn space_manager_for_sixteen_gib() {
    let g = Geometry::for_blocks(4 << 20).unwrap();
    let mut used = Bitmap::new(g.blocks);
    used.set_range(0..1);
    used.set_range(10..10 + 16);
    used.set_range(100..100 + g.pool_blocks);
    used.set_range(70_000..70_010);
    let space = build(&SpaceInput {
        geometry: &g,
        used: &used,
        pool_base: 100,
        ring_base: 10,
        xid: 5,
        spaceman_oid: 1024,
        free_queue_oids: [1027, 1029],
    })
    .unwrap();
    assert_eq!(space.free_blocks, g.blocks - 1 - 16 - g.pool_blocks - 10);
    assert_eq!(space.pool_used, 2 + 2);
    assert!(checksum_ok(&space.spaceman[..]));
    let sm = switch_apfs_format::get::<SpacemanPhys>(&space.spaceman[..], 0).unwrap();
    assert_eq!(sm.fq[1].tree_node_limit.get(), 512);
    assert_eq!(sm.dev[1].addr_offset.get(), 0xa18);
    assert_eq!(cib_addresses(&space.spaceman[..]), vec![100, 101]);
    assert_eq!(&space.spaceman[0x9e0..0x9e2], &[1, 0]);
    assert_eq!(&space.spaceman[0x9e8..0x9ec], &[0xff, 0xff, 0xff, 0xff]);
    assert_eq!(&space.spaceman[0xa04..0xa08], &[0x0f, 0, 0, 0]);
    let pool_bitmap = space.blocks.iter().find(|(a, _)| *a == 11).unwrap();
    assert_eq!(pool_bitmap.1[0], 0x0f);
}
