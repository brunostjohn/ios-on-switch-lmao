// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_format::checksum_ok;
use switch_apfs_format::spaceman::SpacemanPhys;

use crate::bitmap::Bitmap;
use crate::geometry::{Geometry, GeometryError};
use crate::space::{SpaceInput, build, cib_addresses};

/// newfs_apfs's containers on macOS 27.2: pool blocks, queue limits, max volumes.
#[test]
fn geometry_matches_newfs_apfs() {
    for (bytes, chunks, cibs, pool, ip_limit, main_limit, max_fs) in [
        (64u64 << 20, 1, 1, 6, 1, 4, 1),
        (1 << 30, 8, 1, 0x1b, 1, 0x74, 2),
        (16 << 30, 128, 2, 0x186, 1, 0x200, 32),
        (64 << 30, 512, 5, 0x60f, 3, 0x200, 100),
        (1 << 40, 8192, 66, 0x60c6, 0x16, 0x200, 100),
    ] {
        let g = Geometry::for_blocks(bytes / 4096).unwrap();
        assert_eq!(
            (
                g.chunks,
                g.cibs,
                g.pool_blocks,
                g.pool_queue_limit,
                g.main_queue_limit,
                g.max_file_systems
            ),
            (chunks, cibs, pool, ip_limit, main_limit, max_fs),
            "{bytes}"
        );
    }
}

/// The OS images' containers: 24B119 has 2,213,376 blocks and 21H461 1,483,776.
#[test]
fn geometry_matches_apples_sources() {
    let g = Geometry::for_blocks(2_213_376).unwrap();
    assert_eq!((g.chunks, g.cibs, g.pool_blocks, g.max_file_systems), (68, 1, 207, 17));
    let g = Geometry::for_blocks(1_483_776).unwrap();
    assert_eq!((g.chunks, g.cibs, g.pool_blocks, g.max_file_systems), (46, 1, 141, 12));
}

#[test]
fn refuses_what_one_pool_bitmap_cannot_cover() {
    assert!(Geometry::for_blocks(1 << 28).is_ok());
    assert!(matches!(
        Geometry::for_blocks(3 << 27),
        Err(GeometryError::PoolTooLarge(..))
    ));
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
