// SPDX-License-Identifier: GPL-3.0-or-later
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::io::{BlockSource, BlockTarget};
use switch_apfs_write::{Attrs, CheckpointAreas, Content, Fill, Tree};

use crate::parse::{
    parse_areas, parse_int, parse_sealed_cryptex, parse_sha256, parse_uuid, parse_verbatim_cryptex, uuid_string,
};
use crate::plan_dump;

struct EmptySource;

impl BlockSource for EmptySource {
    fn read_blocks(&self, _paddr: u64, out: &mut [u8]) -> std::io::Result<()> {
        out.fill(0);
        Ok(())
    }
}

struct PatternFill(Vec<u8>);

impl Fill for PatternFill {
    fn fill(&self, window: &mut dyn BlockTarget) -> std::io::Result<()> {
        let padded_len = self.0.len().next_multiple_of(BLOCK_SIZE);
        let mut buf = vec![0u8; padded_len];
        buf[..self.0.len()].copy_from_slice(&self.0);
        window.write_blocks(0, &buf)
    }
}

#[test]
fn parses_uuids_ints_areas_and_cryptex_specs() {
    let uuid = parse_uuid("D1B1D053-D0A6-496C-8625-A947A3DA863E").unwrap();
    assert_eq!(uuid_string(&uuid), "D1B1D053-D0A6-496C-8625-A947A3DA863E");
    assert!(parse_uuid("not-a-uuid").is_err());

    assert_eq!(parse_int::<u64>("4096").unwrap(), 4096);
    assert_eq!(parse_int::<u64>("0x200").unwrap(), 0x200);
    assert_eq!(parse_areas("rule").unwrap(), CheckpointAreas::Rule);
    assert_eq!(parse_areas("keep").unwrap(), CheckpointAreas::Keep);
    assert_eq!(parse_areas("mac-composed").unwrap(), CheckpointAreas::MAC_COMPOSED);
    assert_eq!(
        parse_areas("64:1024").unwrap(),
        CheckpointAreas::Sized { desc: 64, data: 1024 }
    );

    let sealed = parse_sealed_cryptex(
        "name=app.dmg,source=/tmp/src.dmg,uuid=D8B9F68E-F191-4FCD-9F89-D88A322E9B4D,manifest=/tmp/m.json,vol_name=App,offset=4096",
    )
    .unwrap();
    assert_eq!(sealed.name, "app.dmg");
    assert_eq!(sealed.offset, 4096);
    assert_eq!(sealed.vol_name.as_deref(), Some("App"));
    assert!(sealed.manifest.is_some());
    assert!(parse_sealed_cryptex("name=app.dmg").is_err());

    let digest = "ab".repeat(32);
    let verb = parse_verbatim_cryptex(&format!("name=v.dmg,path=/tmp/v.dmg,sha256={digest}")).unwrap();
    assert_eq!(verb.name, "v.dmg");
    assert_eq!(verb.sha256, Some(parse_sha256(&digest).unwrap()));
    assert!(parse_verbatim_cryptex("name=v.dmg,path=/tmp/v.dmg,sha256=bad").is_err());
}

#[test]
fn plan_dump_emits_entries_hardlinks_symlinks_xattrs_and_fill_digests() {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let root = tree.root();
    let dir = tree.add_dir(root, "dir", Attrs::new(501, 501, 0o755)).unwrap();

    let s1 = tree.add_bytes(&b"hello world\n"[..]);
    let mut f_attrs = Attrs::new(0, 0, 0o4755);
    f_attrs.bsd_flags = 0x20000;
    let f1 = tree.add_file(dir, "bin", f_attrs, Some(s1)).unwrap();
    tree.add_link(f1, dir, "bin-hard").unwrap();
    tree.set_xattr(f1, "com.example.tag", b"val");

    let payload = vec![0x5Au8; 5000];
    let fill_stream = tree.add_stream(
        payload.len() as u64,
        Content::Fill(Arc::new(PatternFill(payload.clone()))),
    );
    tree.add_file(dir, "filled", Attrs::new(0, 0, 0o644), Some(fill_stream))
        .unwrap();

    tree.add_symlink(dir, "sym", Attrs::new(0, 0, 0o777), "bin").unwrap();

    let time_ns = 1_795_000_000_000_000_000u64;
    let dump = plan_dump::dump_tree(&tree, &EmptySource, time_ns, true).unwrap();
    assert_eq!(dump.version, 1);
    assert_eq!(dump.source, "plan");
    assert_eq!(dump.entries.len(), 6);

    let bin = &dump.entries["/dir/bin"];
    assert_eq!(bin.kind, "f");
    assert_eq!(bin.mode, "4755");
    assert_eq!(bin.flags, 0x20000);
    assert_eq!(bin.nlink, 2);
    assert_eq!(bin.mtime_ns, time_ns);
    assert_eq!(
        bin.sha256.as_deref(),
        Some(hex::encode(Sha256::digest(b"hello world\n")).as_str())
    );
    assert_eq!(bin.xattrs["com.example.tag"].size, 3);
    assert_eq!(
        bin.xattrs["com.example.tag"].sha256,
        hex::encode(Sha256::digest(b"val"))
    );

    let filled = &dump.entries["/dir/filled"];
    assert_eq!(filled.size, 5000);
    assert_eq!(
        filled.sha256.as_deref(),
        Some(hex::encode(Sha256::digest(&payload)).as_str())
    );

    let sym = &dump.entries["/dir/sym"];
    assert_eq!(sym.kind, "l");
    assert_eq!(sym.symlink_target.as_deref(), Some("bin"));
    assert!(sym.xattrs.is_empty());

    assert_eq!(
        dump.hardlink_groups,
        vec![vec!["/dir/bin".to_owned(), "/dir/bin-hard".to_owned()]]
    );

    let dir_entry = &dump.entries["/dir"];
    assert_eq!(dir_entry.size, 32 * (4 + 2));
    assert_eq!(dir_entry.nlink, 2);

    let root_entry = &dump.entries["/"];
    assert_eq!(root_entry.size, 32 * (1 + 2));
    assert_eq!(root_entry.nlink, 3);
}
