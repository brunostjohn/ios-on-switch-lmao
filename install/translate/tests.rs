// SPDX-License-Identifier: GPL-3.0-or-later
//! The pieces that need no Apple bytes. The real check is //install/translate:delta_test against translate_tree.py's
//! deltas.

use std::collections::BTreeMap;
use std::path::PathBuf;

use crate::buffer::Buffer;
use crate::edits::{self, Edit};
use crate::output::Output;
use crate::trust::{self, Pins};
use crate::{CdhashPair, Error, macho, pair_lines, parse_pairs, plan};

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("switch_translate_test.{}.{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    dir
}

#[test]
fn edits_as_load_edits_reads_them() {
    let table = br#"{"build": "x", "files": {"/usr/libexec/d": [
        {"fileoff": "0x7f0a3c", "stock": "AABBccdd", "at": 0, "patched": "11223344"},
        {"fileoff": 16, "stock": "00", "patched": ""},
        {"fileoff": "0b101", "stock": "01"},
        {"fileoff": " 0o17 ", "stock": "02", "at": 1}
    ]}}"#;
    let got = edits::parse(table).unwrap();
    let want = vec![
        Edit {
            fileoff: 0x7f0a3c,
            stock: vec![0xaa, 0xbb, 0xcc, 0xdd],
            at: 0,
            patched: vec![0x11, 0x22, 0x33, 0x44],
        },
        Edit {
            fileoff: 16,
            stock: vec![0],
            at: 0,
            patched: vec![],
        },
        Edit {
            fileoff: 5,
            stock: vec![1],
            at: 0,
            patched: vec![],
        },
        Edit {
            fileoff: 15,
            stock: vec![2],
            at: 1,
            patched: vec![],
        },
    ];
    assert_eq!(got, BTreeMap::from([("/usr/libexec/d".to_string(), want)]));
    assert!(edits::parse(br#"{}"#).unwrap().is_empty());
    for bad in [
        r#"{"files": []}"#,
        r#"{"files": {"rel": [{"fileoff": 0, "stock": "00"}]}}"#,
        r#"{"files": {"/a": []}}"#,
        r#"{"files": {"/a": [{"fileoff": "-1", "stock": "00"}]}}"#,
        r#"{"files": {"/a": [{"fileoff": "010", "stock": "00"}]}}"#,
        r#"{"files": {"/a": [{"fileoff": true, "stock": "00"}]}}"#,
        r#"{"files": {"/a": [{"fileoff": 0, "stock": ""}]}}"#,
        r#"{"files": {"/a": [{"fileoff": 0, "stock": "0"}]}}"#,
        r#"{"files": {"/a": [{"fileoff": 0, "stock": "00", "at": 1, "patched": "11"}]}}"#,
        r#"{"files": {"/a": [{"fileoff": 0, "stock": "00", "at": true}]}}"#,
    ] {
        assert!(matches!(edits::parse(bad.as_bytes()), Err(Error::Edits(_))), "{bad}");
    }
}

#[test]
fn a_file_that_is_no_macho_has_no_slices() {
    let t = macho::translate("/x", &Buffer::zeroed(64), &[]).unwrap();
    assert!(t.slices.is_empty());
    assert_eq!(t.bytes[..], [0u8; 64][..]);
    assert!(!macho::is_macho(b"\xcf\xfa\xed"));
    assert!(macho::is_macho(b"\xca\xfe\xba\xbf...."));
    let edit = Edit {
        fileoff: 0,
        stock: vec![0],
        at: 0,
        patched: vec![],
    };
    assert!(matches!(
        macho::translate("/x", &Buffer::zeroed(64), &[edit]),
        Err(Error::Translate { .. })
    ));
}

#[test]
fn apple_trust_entries() {
    let mut v2 = 2u32.to_le_bytes().to_vec();
    v2.extend_from_slice(&[0; 16]);
    v2.extend_from_slice(&2u32.to_le_bytes());
    for (fill, flags, category) in [(1u8, 0u8, 3u8), (2, 1, 7)] {
        v2.extend_from_slice(&[fill; 20]);
        v2.extend_from_slice(&[2, flags, category, 0]);
    }
    let mut v1 = 1u32.to_le_bytes().to_vec();
    v1.extend_from_slice(&[0; 16]);
    v1.extend_from_slice(&1u32.to_le_bytes());
    v1.extend_from_slice(&[2; 20]);
    v1.extend_from_slice(&[2, 9]);
    let got = trust::categories(&[v2.clone(), v1]).unwrap();
    assert_eq!(got[&[1; 20]], (0, 3));
    assert_eq!(got[&[2; 20]], (9, 0));
    assert!(trust::categories(&[v2[..40].to_vec()]).is_err());
}

#[test]
fn pins_refuse_anything_else() {
    let pins = Pins::from_map(BTreeMap::from([(
        "os.trustcache".to_string(),
        "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824".to_string(),
    )]));
    pins.check("os.trustcache", b"hello").unwrap();
    assert!(matches!(pins.check("os.trustcache", b"hellO"), Err(Error::Refused(_))));
    assert!(matches!(
        pins.check("app_cryptex.trustcache", b"hello"),
        Err(Error::Refused(_))
    ));
    assert!(Pins::parse(b"[1]").is_err());
}

#[test]
fn pairs_round_trip() {
    let pairs = vec![
        CdhashPair {
            old: [0xab; 20],
            now: [0x01; 20],
        },
        CdhashPair {
            old: [0x00; 20],
            now: [0xff; 20],
        },
    ];
    let text = pair_lines(&pairs);
    assert_eq!(
        text.lines().next().unwrap(),
        format!("{} {}", "ab".repeat(20), "01".repeat(20))
    );
    assert_eq!(parse_pairs(&text).unwrap(), pairs);
    assert!(parse_pairs("ab cd\n").is_err());
    assert!(parse_pairs(&format!("{} {} x\n", "ab".repeat(20), "01".repeat(20))).is_err());
}

#[test]
fn output_is_kept_only_when_committed() {
    let out = scratch("dropped");
    {
        let o = Output::create(&out).unwrap();
        o.write("files/usr/lib/x", b"x").unwrap();
        assert!(matches!(o.write("files/../x", b"x"), Err(Error::Refused(_))));
        assert!(matches!(o.write("files//x", b"x"), Err(Error::Refused(_))));
    }
    assert!(!out.exists());
    assert!(
        !out.with_file_name(format!("{}.partial", out.file_name().unwrap().to_string_lossy()))
            .exists()
    );

    let out = scratch("committed");
    let o = Output::create(&out).unwrap();
    o.write("a/b", b"bytes").unwrap();
    assert_eq!(o.commit().unwrap(), out);
    assert_eq!(std::fs::read(out.join("a/b")).unwrap(), b"bytes");
    assert!(matches!(Output::create(&out), Err(Error::Refused(_))));
    std::fs::remove_dir_all(&out).unwrap();
}

/// A main cache file naming a subcache that isn't given: the planner gives up in the middle of a run (its exit) and
/// must come back clean for the next run.
#[test]
fn the_planner_refuses_a_missing_subcache_twice_alike() {
    let mut main = Buffer::zeroed(4096);
    main[0x188..0x18c].copy_from_slice(&0x200u32.to_le_bytes());
    main[0x18c..0x190].copy_from_slice(&1u32.to_le_bytes());
    main[0x200 + 24..0x200 + 27].copy_from_slice(b".01");
    let mut logs = Vec::new();
    for _ in 0..2 {
        match plan::make("cache", &[("", &main[..])]) {
            Err(Error::Plan(log)) => logs.push(log),
            Err(other) => panic!("refused for another reason: {other}"),
            Ok(_) => panic!("planned without its subcache"),
        }
    }
    assert!(logs[0].contains("cache.01: not among the cache's files"), "{}", logs[0]);
    assert_eq!(logs[0], logs[1]);
}
