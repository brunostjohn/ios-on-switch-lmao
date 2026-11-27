// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::fs;

use serde_json::json;
use switch_seed_crawl::{Constant, Function, KERNEL};
use tempfile::TempDir;

use crate::{
    CACHE_ENTRY_FORMAT, CACHE_ENTRY_MAGIC, DEFAULT_BUCKET_BYTES, Depth, EntrySource, IDENTITY_BYTES, PAGE_BYTES,
    SLOT_PAGE_BYTES, SeedPackEntry, SeedPackReader, bucket_lines, cache_key_name, kernels, layout_seed_tree,
    lines_for_depth, neighbours, ordered, pack_lists, pack_native_seed, parse_kernel, parse_render,
    parse_specialization, restamp_buckets, seed_slot_name, staged_in_pack, targets, write_seed_pack,
};

#[test]
fn journal_roundtrip_and_constant_order() {
    let line = "R ok=1 label=com.apple.coreanimation.A samples=1 c0=80:15:0:1:0:0:1:0:0 a0=28:0:0:16:0:0 | v vert_fn lib=/A.metallib #2:29:01000000 #0:53:01 | f frag_fn lib=/A.metallib spec | fl link_fn lib=/B.metallib | ";
    let parsed = parse_render(line).unwrap();
    assert_eq!(parsed.colors(), 1);
    assert_eq!(parsed.linked.len(), 1);
    assert_eq!(parsed.text(), line);

    let sorted = ordered(&[
        "z_name:53:01".to_owned(),
        "#10:29:00".to_owned(),
        "#2:29:00".to_owned(),
        "a_name:53:00".to_owned(),
    ]);
    assert_eq!(sorted, vec!["#2:29:00", "#10:29:00", "a_name:53:00", "z_name:53:01"]);

    let spec = parse_specialization("S ok=1 lib=/X.metallib name=fn_a k=c:53:01 | ").unwrap();
    assert!(spec.spec);
    assert_eq!(spec.lib.as_deref(), Some("/X.metallib"));
    assert_eq!(spec.constants, vec!["c:53:01"]);

    let (fields, kern) =
        parse_kernel("D ok=1 tg=8,8,1 edge=0,0,0 threads=0 | form=f opt=0 | k my_k lib=/K.metallib | ").unwrap();
    assert_eq!(fields, "form=f opt=0");
    assert_eq!(kern.name, "my_k");
}

#[test]
fn derive_kernels_targets_neighbours_and_buckets() {
    let mut index = BTreeMap::new();
    index.insert(
        "/K.metallib".to_owned(),
        vec![
            Function {
                name: "k_seen".to_owned(),
                kind: KERNEL,
                constants: vec![],
                air: String::new(),
                hash: None,
            },
            Function {
                name: "k_bool".to_owned(),
                kind: KERNEL,
                constants: vec![Constant {
                    name: "flag".to_owned(),
                    data_type: 53,
                    index: 0,
                    required: true,
                }],
                air: String::new(),
                hash: None,
            },
        ],
    );
    let corpus = vec![
        "C ok=1 form=f opt=0 | k k_seen lib=/K.metallib | ".to_owned(),
        "R ok=1 label=L1 samples=1 c0=80:15:0:1:0:0:1:0:0 | v vs lib=/R.metallib #0:53:00 | f fs lib=/R.metallib | "
            .to_owned(),
        "R ok=1 label=L2 samples=1 c0=115:15:0:1:0:0:1:0:0 | v vs lib=/R.metallib #0:53:01 | f fs lib=/R.metallib | "
            .to_owned(),
    ];
    let ks = kernels(&index, corpus.iter().map(String::as_str));
    assert_eq!(ks.len(), 2);
    assert!(ks[0].contains("k_bool") && ks[0].contains("flag:53:00"));
    assert!(ks[1].contains("k_bool") && ks[1].contains("flag:53:01"));

    let renders: Vec<_> = corpus.iter().filter_map(|l| parse_render(l)).collect();
    assert_eq!(targets(&renders).len(), 2);
    assert_eq!(neighbours(&renders).len(), 2);

    let tiers = lines_for_depth(Depth::Deep, &corpus, &index);
    let (buckets, kept) = bucket_lines(&tiers, 4, 0).unwrap();
    assert_eq!(buckets.len(), 4);
    assert_eq!(kept.get(&("corpus".to_owned(), 'R')), Some(&2));
    assert_eq!(kept.get(&("kernels".to_owned(), 'C')), Some(&2));
    assert!(bucket_lines(&tiers, 0, 0).is_err());
    assert!(bucket_lines(&tiers, 5000, 0).is_err());
}

#[test]
fn seedpack_roundtrip_and_page_alignment() {
    let dir = TempDir::new().unwrap();
    let identity = [0x5Au8; IDENTITY_BYTES];
    let slot_bytes = vec![0xABu8; PAGE_BYTES as usize];
    let entries = vec![
        SeedPackEntry {
            name: vec![0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08],
            tier: 0,
            source: EntrySource::Bytes(b"observed-rec".to_vec()),
        },
        SeedPackEntry {
            name: vec![0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02],
            tier: 0,
            source: EntrySource::Bytes(slot_bytes.clone()),
        },
        SeedPackEntry {
            name: vec![0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8],
            tier: 1,
            source: EntrySource::Bytes(b"derived-rec".to_vec()),
        },
    ];
    write_seed_pack(dir.path(), 8, &identity, entries, DEFAULT_BUCKET_BYTES).unwrap();
    let reader = SeedPackReader::open(dir.path(), 8, Some(&identity)).unwrap();
    assert_eq!(reader.observed_packs(), 1);
    assert_eq!(reader.pack_count(), 2);
    assert_eq!(
        reader.find(&[0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]).unwrap(),
        b"observed-rec"
    );
    assert_eq!(
        reader.find(&[0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02]).unwrap(),
        slot_bytes.as_slice()
    );
    assert!(staged_in_pack(dir.path(), "index").unwrap());
    assert!(staged_in_pack(dir.path(), "packs/000").unwrap());
    assert!(!staged_in_pack(dir.path(), "packs/001").unwrap());
}

fn write_captured_program(out: &mut Vec<u8>, image: &[u8], data: &[u8]) {
    let id = b"prog0";
    out.extend_from_slice(&(id.len() as u32).to_le_bytes());
    out.extend_from_slice(id);
    out.extend_from_slice(&(image.len() as u32).to_le_bytes());
    out.extend_from_slice(image);
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    out.extend_from_slice(&0u32.to_le_bytes()); // cbufTypes
    out.extend_from_slice(&0u32.to_le_bytes()); // farUbos
    out.push(16); // gprs
    out.push(0); // boundTextures
}

#[test]
fn restamp_rewrites_stack_and_extracts_large_slot() {
    let bucket = TempDir::new().unwrap();
    let stamped = TempDir::new().unwrap();
    let packed = TempDir::new().unwrap();
    let tree = TempDir::new().unwrap();

    let old_stack = 0u64;
    let v_air = [0x11u8; 32];
    let f_air = [0x22u8; 32];
    let key = "samples=1 | v vs | f fs";
    let old_name = format!("{:016x}", cache_key_name(old_stack, &v_air, &f_air, key));

    let mut rec = Vec::new();
    rec.extend_from_slice(CACHE_ENTRY_MAGIC);
    rec.extend_from_slice(&CACHE_ENTRY_FORMAT.to_le_bytes());
    rec.extend_from_slice(&1u32.to_le_bytes()); // KIND_RECORD
    rec.extend_from_slice(&old_stack.to_le_bytes());
    rec.extend_from_slice(&v_air);
    rec.extend_from_slice(&f_air);
    rec.extend_from_slice(&(key.len() as u32).to_le_bytes());
    rec.extend_from_slice(key.as_bytes());
    rec.extend_from_slice(&vec![0u8; 736]); // PackPipeline
    rec.extend_from_slice(&0u32.to_le_bytes()); // keyString
    let big_image = vec![0x42u8; SLOT_PAGE_BYTES + 256];
    write_captured_program(&mut rec, &big_image, &[]);
    write_captured_program(&mut rec, &[0x11u8; 96], &[]);
    rec.extend_from_slice(&0u32.to_le_bytes()); // blob
    rec.extend_from_slice(&0u32.to_le_bytes()); // extras
    rec.extend_from_slice(&0u32.to_le_bytes()); // restore
    rec.extend_from_slice(&0u32.to_le_bytes()); // samplers
    rec.push(0); // split bits
    rec.extend_from_slice(&0u32.to_le_bytes()); // variants
    rec.extend_from_slice(&0u32.to_le_bytes()); // spill
    rec.extend_from_slice(&0u32.to_le_bytes()); // far
    rec.push(0); // strideTwin

    fs::create_dir_all(bucket.path().join("rec-0000000000000000")).unwrap();
    let old_rel = format!("rec-0000000000000000/{old_name}.rec");
    fs::write(bucket.path().join(&old_rel), &rec).unwrap();
    fs::write(
        bucket.path().join("manifest.json"),
        serde_json::to_string(&json!({
            "schema": 1,
            "stack": "0000000000000000",
            "packVersion": 37,
            "cacheVersion": CACHE_ENTRY_FORMAT,
            "records": [{
                "status": "made",
                "derived": false,
                "key": key,
                "name": old_name,
                "vertexAir": hex::encode(v_air),
                "fragmentAir": hex::encode(f_air),
                "artifact": old_rel,
            }]
        }))
        .unwrap(),
    )
    .unwrap();

    let manifest = restamp_buckets("0123456789abcdef", stamped.path(), &[bucket.path().to_path_buf()]).unwrap();
    let lists = pack_lists(&manifest).unwrap();
    assert_eq!(lists.len(), 2); // .rec + extracted .slot

    let mut expected_slot = vec![0u8; 0x30];
    expected_slot.extend_from_slice(&big_image);
    expected_slot.resize((expected_slot.len() + 0xFF) & !0xFF, 0);
    let slot_hex = format!("{:016x}", seed_slot_name(&expected_slot));
    assert!(lists.contains_key(&slot_hex));

    let packed_dir = pack_native_seed(stamped.path(), packed.path()).unwrap();
    assert!(packed_dir.join("index").is_file());
    let size = layout_seed_tree(None, None, Some(packed.path()), tree.path()).unwrap();
    assert_eq!(size.files, 2); // index + packs/000
}

#[test]
fn seedpack_zero_byte_tier_and_traversal_refusal() {
    let dir = TempDir::new().unwrap();
    let identity = [0x33u8; IDENTITY_BYTES];
    let entries = vec![SeedPackEntry {
        name: vec![0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08],
        tier: 0,
        source: EntrySource::Bytes(Vec::new()),
    }];
    write_seed_pack(dir.path(), 8, &identity, entries, DEFAULT_BUCKET_BYTES).unwrap();
    let reader = SeedPackReader::open(dir.path(), 8, Some(&identity)).unwrap();
    assert_eq!(
        reader.find(&[0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]).unwrap(),
        b""
    );

    let bad_manifest = json!({
        "schema": 1,
        "stack": "0123456789abcdef",
        "records": [{
            "status": "made",
            "derived": false,
            "name": "0011223344556677",
            "artifact": "../../etc/passwd",
        }]
    });
    assert!(pack_lists(&bad_manifest).is_err());
}

fn synthetic_mtlb(fn_name: &str, const_name: &str) -> Vec<u8> {
    let mut cnst = Vec::new();
    cnst.extend_from_slice(&1u16.to_le_bytes());
    cnst.extend_from_slice(const_name.as_bytes());
    cnst.push(0);
    cnst.push(53); // bool
    cnst.extend_from_slice(&0u16.to_le_bytes());
    cnst.push(1); // required

    let mut pub_body = Vec::new();
    pub_body.extend_from_slice(b"CNST");
    pub_body.extend_from_slice(&(cnst.len() as u16).to_le_bytes());
    pub_body.extend_from_slice(&cnst);
    pub_body.extend_from_slice(b"ENDT\0\0");

    let mut pub_sec = Vec::new();
    pub_sec.extend_from_slice(&(pub_body.len() as u32).to_le_bytes());
    pub_sec.extend_from_slice(&pub_body);

    let mut tags = Vec::new();
    let mut name_nul = fn_name.as_bytes().to_vec();
    name_nul.push(0);
    tags.extend_from_slice(b"NAME");
    tags.extend_from_slice(&(name_nul.len() as u16).to_le_bytes());
    tags.extend_from_slice(&name_nul);
    tags.extend_from_slice(b"TYPE");
    tags.extend_from_slice(&1u16.to_le_bytes());
    tags.push(KERNEL);
    tags.extend_from_slice(b"OFFT");
    tags.extend_from_slice(&8u16.to_le_bytes());
    tags.extend_from_slice(&0u64.to_le_bytes());
    tags.extend_from_slice(b"ENDT\0\0");

    let entry_size = (4 + tags.len()) as u32;
    let mut list_sec = Vec::new();
    list_sec.extend_from_slice(&1u32.to_le_bytes());
    list_sec.extend_from_slice(&entry_size.to_le_bytes());
    list_sec.extend_from_slice(&tags);

    let hdr_len = 0x58usize;
    let public_off = hdr_len + list_sec.len();
    let total_len = public_off + pub_sec.len();
    let mut buf = vec![0u8; hdr_len];
    buf[0..4].copy_from_slice(b"MTLB");
    buf[0x10..0x18].copy_from_slice(&(total_len as u64).to_le_bytes());
    buf[0x18..0x20].copy_from_slice(&(hdr_len as u64).to_le_bytes());
    buf[0x28..0x30].copy_from_slice(&(public_off as u64).to_le_bytes());
    buf.extend_from_slice(&list_sec);
    buf.extend_from_slice(&pub_sec);
    buf
}

#[test]
fn planner_indexes_build_specific_air_slice_on_fat_metallib() {
    let slice_hi = synthetic_mtlb("k_hi", "c_hi");
    let slice_27 = synthetic_mtlb("k_27", "c_27");
    let off0 = 8 + 40;
    let off1 = off0 + slice_hi.len();
    let mut fat = Vec::new();
    fat.extend_from_slice(switch_seed_crawl::FAT_MAGIC);
    fat.extend_from_slice(&2u32.to_be_bytes());
    for (sub, off, len) in [(0x20009u32, off0, slice_hi.len()), (0xCu32, off1, slice_27.len())] {
        fat.extend_from_slice(&switch_seed_crawl::AIR64_CPU_TYPE.to_be_bytes());
        fat.extend_from_slice(&sub.to_be_bytes());
        fat.extend_from_slice(&(off as u32).to_be_bytes());
        fat.extend_from_slice(&(len as u32).to_be_bytes());
        fat.extend_from_slice(&0u32.to_be_bytes());
    }
    fat.extend_from_slice(&slice_hi);
    fat.extend_from_slice(&slice_27);

    let tmp = TempDir::new().unwrap();
    fs::write(tmp.path().join("fat.metallib"), &fat).unwrap();

    let (idx_17, _) = crate::index_libraries(tmp.path(), &[], Some("21H461")).unwrap();
    assert_eq!(idx_17["/fat.metallib"][0].name, "k_hi");
    assert_eq!(idx_17["/fat.metallib"][0].constants[0].name, "c_hi");

    let (idx_27, _) = crate::index_libraries(tmp.path(), &[], Some("24B119")).unwrap();
    assert_eq!(idx_27["/fat.metallib"][0].name, "k_27");
    assert_eq!(idx_27["/fat.metallib"][0].constants[0].name, "c_27");
}
