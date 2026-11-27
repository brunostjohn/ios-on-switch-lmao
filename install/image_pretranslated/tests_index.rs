// SPDX-License-Identifier: GPL-3.0-or-later
use crate::overlay_index::{
    self, CDHASH_LEN, FILE_RECORD_SIZE, HEADER_SIZE, IndexEntry, IndexError, ORIGINALS_HEADER_SIZE, OriginalsPair,
    cdhash_from_slice,
};
use crate::{Manifest, ManifestFile};

fn cd_seq() -> [u8; CDHASH_LEN] {
    std::array::from_fn(|i| i as u8)
}

#[test]
fn originals_layout_dedup_and_refusals() {
    let raw = overlay_index::encode_originals(&[
        OriginalsPair {
            original: [9; 20],
            translated: [1; 20],
        },
        OriginalsPair {
            original: [4; 20],
            translated: [2; 20],
        },
    ])
    .unwrap();
    assert_eq!(&raw[0..4], b"SWOR");
    assert_eq!(u32::from_le_bytes(raw[4..8].try_into().unwrap()), 1);
    assert_eq!(u32::from_le_bytes(raw[8..12].try_into().unwrap()), 2);
    assert_eq!(u32::from_le_bytes(raw[12..16].try_into().unwrap()), 0);
    let mut want_body = Vec::new();
    want_body.extend_from_slice(&[4; 20]);
    want_body.extend_from_slice(&[2; 20]);
    want_body.extend_from_slice(&[9; 20]);
    want_body.extend_from_slice(&[1; 20]);
    assert_eq!(&raw[ORIGINALS_HEADER_SIZE..], &want_body[..]);
    assert_eq!(
        overlay_index::decode_originals(&raw).unwrap(),
        vec![
            OriginalsPair {
                original: [4; 20],
                translated: [2; 20],
            },
            OriginalsPair {
                original: [9; 20],
                translated: [1; 20],
            },
        ]
    );

    let pair = OriginalsPair {
        original: [4; 20],
        translated: [2; 20],
    };
    assert_eq!(
        overlay_index::encode_originals(&[pair, pair]).unwrap(),
        overlay_index::encode_originals(&[pair]).unwrap()
    );

    assert!(matches!(
        overlay_index::encode_originals(&[
            OriginalsPair {
                original: [0; 20],
                translated: [1; 20],
            },
            OriginalsPair {
                original: [0; 20],
                translated: [2; 20],
            },
        ]),
        Err(IndexError::ConflictingOriginal(_))
    ));

    assert!(matches!(cdhash_from_slice(&[0; 19]), Err(IndexError::BadCdhashLength)));
    assert!(matches!(cdhash_from_slice(&[0; 21]), Err(IndexError::BadCdhashLength)));
    assert!(matches!(cdhash_from_slice(&[]), Err(IndexError::BadCdhashLength)));
}

#[test]
fn overlay_index_layout_and_round_trip() {
    let cd = cd_seq();
    let files = vec![
        IndexEntry {
            path: "/usr/libexec/b".into(),
            system_id: 30,
            data_id: 300,
            cdhash: [3; 20],
        },
        IndexEntry {
            path: "/System/Library/a".into(),
            system_id: 10,
            data_id: 100,
            cdhash: cd,
        },
        IndexEntry {
            path: "/usr/libexec/a".into(),
            system_id: 20,
            data_id: 200,
            cdhash: [2; 20],
        },
    ];
    let raw = overlay_index::encode(&files, &[7, 5]).unwrap();
    assert_eq!(&raw[0..4], b"SWOV");
    assert_eq!(u32::from_le_bytes(raw[4..8].try_into().unwrap()), 2);
    assert_eq!(u32::from_le_bytes(raw[8..12].try_into().unwrap()), 3);
    assert_eq!(u32::from_le_bytes(raw[12..16].try_into().unwrap()), 2);
    assert_eq!(u32::from_le_bytes(raw[20..24].try_into().unwrap()), 0);

    let recs: Vec<(u64, u64, u32, u32)> = (0..3)
        .map(|i| {
            let b = HEADER_SIZE + FILE_RECORD_SIZE * i;
            (
                u64::from_le_bytes(raw[b..b + 8].try_into().unwrap()),
                u64::from_le_bytes(raw[b + 8..b + 16].try_into().unwrap()),
                u32::from_le_bytes(raw[b + 16..b + 20].try_into().unwrap()),
                u32::from_le_bytes(raw[b + 20..b + 24].try_into().unwrap()),
            )
        })
        .collect();
    assert_eq!(recs, [(10, 100, 0, 0), (20, 200, 18, 0), (30, 300, 33, 0)]);
    assert_eq!(&raw[HEADER_SIZE + 24..HEADER_SIZE + 44], &cd);

    let at = HEADER_SIZE + FILE_RECORD_SIZE * 3;
    assert_eq!(u64::from_le_bytes(raw[at..at + 8].try_into().unwrap()), 5);
    assert_eq!(u64::from_le_bytes(raw[at + 8..at + 16].try_into().unwrap()), 7);
    assert_eq!(&raw[at + 16..], b"/System/Library/a\0/usr/libexec/a\0/usr/libexec/b\0");

    let rt_files = vec![
        IndexEntry {
            path: "/a".into(),
            system_id: 1,
            data_id: 11,
            cdhash: [1; 20],
        },
        IndexEntry {
            path: "/b/c".into(),
            system_id: 2,
            data_id: 12,
            cdhash: [2; 20],
        },
        IndexEntry {
            path: "/z".into(),
            system_id: 4,
            data_id: 14,
            cdhash: [4; 20],
        },
    ];
    let encoded = overlay_index::encode(&rt_files, &[20, 21]).unwrap();
    assert_eq!(overlay_index::decode(&encoded).unwrap(), (rt_files, vec![20, 21]));
}

#[test]
fn overlay_index_refuses_bad_paths_ids_and_loose_tables() {
    let cd = cd_seq();
    let long_path = format!("/{}", "x".repeat(1024));
    for bad in [
        "relative",
        "/trailing/",
        "/a//b",
        "/a/./b",
        "/a/../b",
        long_path.as_str(),
        "/.index",
        "/a\0b",
    ] {
        let entry = IndexEntry {
            path: bad.into(),
            system_id: 1,
            data_id: 2,
            cdhash: cd,
        };
        assert!(overlay_index::encode(&[entry], &[3]).is_err(), "{bad} should fail");
    }

    assert!(matches!(
        overlay_index::encode(
            &[
                IndexEntry {
                    path: "/a".into(),
                    system_id: 1,
                    data_id: 2,
                    cdhash: cd,
                },
                IndexEntry {
                    path: "/a".into(),
                    system_id: 3,
                    data_id: 4,
                    cdhash: cd,
                },
            ],
            &[5],
        ),
        Err(IndexError::DuplicatePath(_))
    ));

    let bad_id_cases: Vec<(Vec<IndexEntry>, Vec<u64>)> = vec![
        (
            vec![IndexEntry {
                path: "/a".into(),
                system_id: 0,
                data_id: 2,
                cdhash: cd,
            }],
            vec![3],
        ),
        (
            vec![IndexEntry {
                path: "/a".into(),
                system_id: 1,
                data_id: 2,
                cdhash: cd,
            }],
            vec![0],
        ),
        (
            vec![
                IndexEntry {
                    path: "/a".into(),
                    system_id: 1,
                    data_id: 2,
                    cdhash: cd,
                },
                IndexEntry {
                    path: "/b".into(),
                    system_id: 1,
                    data_id: 4,
                    cdhash: cd,
                },
            ],
            vec![5],
        ),
        (
            vec![
                IndexEntry {
                    path: "/a".into(),
                    system_id: 1,
                    data_id: 2,
                    cdhash: cd,
                },
                IndexEntry {
                    path: "/b".into(),
                    system_id: 1,
                    data_id: 2,
                    cdhash: [0; 20],
                },
            ],
            vec![5],
        ),
        (
            vec![
                IndexEntry {
                    path: "/a".into(),
                    system_id: 1,
                    data_id: 2,
                    cdhash: cd,
                },
                IndexEntry {
                    path: "/b".into(),
                    system_id: 3,
                    data_id: 2,
                    cdhash: cd,
                },
            ],
            vec![5],
        ),
        (
            vec![IndexEntry {
                path: "/a".into(),
                system_id: 1,
                data_id: 2,
                cdhash: cd,
            }],
            vec![2],
        ),
        (
            vec![IndexEntry {
                path: "/a".into(),
                system_id: 1,
                data_id: 2,
                cdhash: cd,
            }],
            vec![5, 5],
        ),
    ];
    for (files, dirs) in bad_id_cases {
        assert!(overlay_index::encode(&files, &dirs).is_err());
    }

    let hl = vec![
        IndexEntry {
            path: "/usr/libexec/d".into(),
            system_id: 1,
            data_id: 2,
            cdhash: cd,
        },
        IndexEntry {
            path: "/usr/sbin/d".into(),
            system_id: 1,
            data_id: 2,
            cdhash: cd,
        },
        IndexEntry {
            path: "/usr/sbin/e".into(),
            system_id: 3,
            data_id: 4,
            cdhash: cd,
        },
    ];
    assert_eq!(
        overlay_index::decode(&overlay_index::encode(&hl, &[5]).unwrap()).unwrap(),
        (hl, vec![5])
    );

    let mut bad_off = overlay_index::encode(
        &[
            IndexEntry {
                path: "/a".into(),
                system_id: 1,
                data_id: 2,
                cdhash: cd,
            },
            IndexEntry {
                path: "/b".into(),
                system_id: 3,
                data_id: 4,
                cdhash: cd,
            },
        ],
        &[5],
    )
    .unwrap();
    let off = HEADER_SIZE + FILE_RECORD_SIZE + 16;
    bad_off[off..off + 4].copy_from_slice(&0u32.to_le_bytes());
    assert!(overlay_index::decode(&bad_off).is_err());

    let mut swapped = Vec::new();
    swapped.extend_from_slice(b"SWOV");
    swapped.extend_from_slice(&2u32.to_le_bytes());
    swapped.extend_from_slice(&2u32.to_le_bytes());
    swapped.extend_from_slice(&1u32.to_le_bytes());
    swapped.extend_from_slice(&6u32.to_le_bytes());
    swapped.extend_from_slice(&0u32.to_le_bytes());
    for (s, d, o) in [(1u64, 2u64, 0u32), (3, 4, 3)] {
        swapped.extend_from_slice(&s.to_le_bytes());
        swapped.extend_from_slice(&d.to_le_bytes());
        swapped.extend_from_slice(&o.to_le_bytes());
        swapped.extend_from_slice(&0u32.to_le_bytes());
        swapped.extend_from_slice(&cd);
    }
    swapped.extend_from_slice(&5u64.to_le_bytes());
    swapped.extend_from_slice(b"/b\0/a\0");
    assert!(overlay_index::decode(&swapped).is_err());

    let short = overlay_index::encode(
        &[IndexEntry {
            path: "/a".into(),
            system_id: 1,
            data_id: 2,
            cdhash: cd,
        }],
        &[5],
    )
    .unwrap();
    assert!(overlay_index::decode(&short[..short.len() - 1]).is_err());
}

#[test]
fn pretranslated_json_matches_python_dumps() {
    assert_eq!(Manifest { files: vec![] }.to_json(), "{\n \"files\": []\n}\n");

    let m1 = Manifest {
        files: vec![ManifestFile {
            path: "/a/\u{e9}\u{1f600}\"\t\0\u{7f}".into(),
            trust: true,
            mode: "0755".into(),
            sha256: "11".repeat(32),
            cdhash: vec![],
        }],
    };
    assert_eq!(
        m1.to_json(),
        concat!(
            "{\n \"files\": [\n  {\n",
            "   \"path\": \"/a/\\u00e9\\ud83d\\ude00\\\"\\t\\u0000\\u007f\",\n",
            "   \"trust\": true,\n",
            "   \"mode\": \"0755\",\n",
            "   \"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\",\n",
            "   \"cdhash\": []\n",
            "  }\n ]\n}\n",
        )
    );

    let m2 = Manifest {
        files: vec![ManifestFile {
            path: "/usr/libexec/tool".into(),
            trust: true,
            mode: "4755".into(),
            sha256: "ab".repeat(32),
            cdhash: vec!["aa".repeat(20), "bb".repeat(20)],
        }],
    };
    assert_eq!(
        m2.to_json(),
        concat!(
            "{\n \"files\": [\n  {\n",
            "   \"path\": \"/usr/libexec/tool\",\n",
            "   \"trust\": true,\n",
            "   \"mode\": \"4755\",\n",
            "   \"sha256\": \"abababababababababababababababababababababababababababababababab\",\n",
            "   \"cdhash\": [\n",
            "    \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n",
            "    \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n",
            "   ]\n",
            "  }\n ]\n}\n",
        )
    );
}
