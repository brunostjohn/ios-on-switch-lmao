// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap, HashSet};

use crate::archives::{compute_key, declared, section_bytes, state_key};
use crate::fetchfree::{fragment_banks, ldc_loads};
use crate::headers::{apply_draw_context, chip_facts, color_table_words, own_states, owned_method};
use crate::iterated_long::iterated_programs;
use crate::iterated_ucode::half_bits;
use crate::kernels::{KernelEntry, views_other_texel_size};
use crate::packfile::{MAGIC, Pack, PipelineEntry, PipelineRecord, VERSION, key_hash_str, variant_name};
use crate::packfile_encode::{pack_bytes, stamp_pack_bytes};
use crate::records::fnv1a64;
use crate::segments::slm_per_warp_of_header;
use crate::splits;

#[test]
fn headers_single_source_of_truth_parsed() {
    assert!(owned_method(0x0000));
    assert!(owned_method(0x1C04));
    assert!(!owned_method(0x1C00));
    assert_eq!(chip_facts(), 0x40000);
    assert!(own_states().contains_key("two_plane_cover | f two_plane_load"));
    assert_eq!(color_table_words().len(), 1024);

    let mut post = BTreeMap::new();
    post.insert(0x0F90, 123);
    let ctx = apply_draw_context(&post, "360 20164010\nf90 -\n");
    assert_eq!(ctx.get(&0x0360), Some(&0x2016_4010));
    assert!(!ctx.contains_key(&0x0F90));
}

#[test]
fn iterated_half_bits_and_programs() {
    assert_eq!(half_bits(0.25), 0x3400);
    assert_eq!(half_bits(1.086_400_032_043_457 - 1.0), 0x2D88);
    assert_eq!(half_bits(0.0), 0x0000);
    assert_eq!(half_bits(-0.5), 0xB800);

    let progs = iterated_programs();
    assert_eq!(progs.len(), 36);
    assert!(progs.contains_key("mpy"));
    assert!(progs.contains_key("cli"));
    assert!(progs.contains_key("hue"));
    assert!(progs.contains_key("dlc.u"));
    // hsl first pass uses EPS (k=1), which first_pass_on_constant_zero swaps to k=0.
    let hue = &progs["hue"];
    assert_eq!((hue.words[0] >> 6) & 7, 0);
    assert_eq!(hue.constants[&0], [0x068E, 0x068E, 0x068E]);
}

#[test]
fn archives_state_and_compute_keys() {
    let r = "R ok=1 label=X samples=1 c0=80:15:0:1:0:0:1:0:0 | v vs lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib spec | f fs lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib | ";
    assert_eq!(
        state_key(r),
        "samples=1 c0=80:15:0:1:0:0:1:0:0  | v vs | f fs | l RenderBox"
    );
    let c = "C ok=1 max=256 tgm=16 | k my_kern lib=/X.metallib spec a:53:01 | ";
    assert_eq!(compute_key(c), "k my_kern a:53:01 | max=256 tgm=16");

    let decl = declared(
        &format!("render\tRB\t/a.bin\tdevice\t{r}\ncompute\tRB\t/a.bin\tdevice\t{c}\n"),
        &HashSet::new(),
    );
    assert_eq!(decl.render_rows, 1);
    assert_eq!(decl.compute_rows, 1);
    let sec = section_bytes(&decl, &Default::default());
    assert_eq!(u32::from_le_bytes(sec[0..4].try_into().unwrap()), 2);
}

#[test]
fn kernel_texel_view_and_fetchfree_ldc() {
    let entries = vec![KernelEntry {
        off: 0,
        kind: 1,
        slot: 0,
        window: 0x07, // 4-byte texel
        residual: None,
    }];
    assert!(views_other_texel_size("kernel | tex=0:80 | ", &entries).is_empty());
    assert_eq!(views_other_texel_size("kernel | tex=0:10 | ", &entries), vec![0]);

    let mut image = vec![0u8; 0x50 + 32];
    // Slot 1 (offset 0x50 + 8): LDC bank=3, count=1 (size code 4), offset=0x60.
    let ldc: u64 = (0x1DF2u64 << 51) | (4u64 << 48) | (3u64 << 36) | (0x60u64 << 20);
    image[0x50 + 8..0x50 + 16].copy_from_slice(&ldc.to_le_bytes());
    let loaded = ldc_loads(&image, 3);
    assert!(loaded.contains(&0x60));
    assert_eq!(loaded.len(), 1);
}

#[test]
fn split_record_decode_and_slm_and_stamp() {
    let mut split_raw = vec![0u8; 322];
    split_raw[0..4].copy_from_slice(&128u32.to_le_bytes());
    split_raw[16..18].copy_from_slice(&64u16.to_le_bytes());
    split_raw[18] = 3;
    split_raw[19] = 1;
    split_raw[22] = 2;
    let rec = splits::of_record(&split_raw, "fs".into(), "pro".into()).unwrap();
    assert_eq!(rec.threshold, 128);
    assert_eq!(rec.frontier_bytes, 64);
    assert_eq!(rec.ubo_cbufs, vec![2]);
    assert!(splits::of_record(&split_raw[..16], "fs".into(), "pro".into()).is_err());

    let mut sph = vec![0u8; 32];
    sph[4..8].copy_from_slice(&64u32.to_le_bytes());
    assert_eq!(slm_per_warp_of_header(&sph, 0), 2048);

    assert_eq!(
        variant_name("samples=1 | v a | f b | tests=3 | xrs=1"),
        " | tests=3 | xrs=1"
    );
    assert_eq!(
        key_hash_str("samples=1 | v two_plane_cover | f two_plane_load", ""),
        fnv1a64(b"raw.two_plane.two_plane_load")
    );

    let pack = Pack {
        pipes: Vec::new(),
        programs: Vec::new(),
        constexpr_tsc: Vec::new(),
        baseline: BTreeMap::new(),
        init: Vec::new(),
        heap: Vec::new(),
        kernels: Vec::new(),
        splits: Vec::new(),
        archives: Vec::new(),
        iterated: Vec::new(),
        compute_facts: Vec::new(),
        reflections: BTreeMap::new(),
        fast_keys: HashMap::new(),
        tier0_programs: Some(0),
        tier0_heap: Some(0),
        segments: Vec::new(),
        tier0_slm_per_warp: 0,
    };
    let (mut bytes, _) = pack_bytes(&pack, None).unwrap();
    assert_eq!(&bytes[..8], MAGIC);
    assert_eq!(u32::from_le_bytes(bytes[8..12].try_into().unwrap()), VERSION);
    stamp_pack_bytes(&mut bytes, 0xDEAD_BEEF_CAFE_BABE).unwrap();
    let stack_off = 8 + 16 + 16 * 17;
    assert_eq!(
        u64::from_le_bytes(bytes[stack_off..stack_off + 8].try_into().unwrap()),
        0xDEAD_BEEF_CAFE_BABE
    );

    let pipe = PipelineRecord {
        index: 0,
        key: String::new(),
        form: String::new(),
        tier: 0,
        vs: 0,
        fs: 0,
        blob: vec![],
        restore: vec![],
        tess: [0, 0],
        runtime_head: [0; 7],
        pulled_stride: [0; 16],
        stride_twin: 0,
        amplification_layer_word: 0,
        amplification_viewport_word: 0,
        guest_base_word: [0; 4],
        guest_base_slot: [0; 4],
        push: vec![],
        push_metadata: false,
        turn_refused: false,
        raster_ordered: false,
        half_blend_color_word: 0,
        iterated: 0,
        iterated_name: String::new(),
        fs_image: vec![],
        xr_slots: 0,
        storage_mask: 0,
        storage_write_mask: 0,
        storage_samples: 0,
        storage_tests: 0,
        storage_blend_mask: 0,
        storage_blend_model: 0,
        depth_test_word: 0,
        stencil_test_word: 0,
        test_flags: 0,
        shadow_stencil_word: 0,
        depth_tic: 0,
        stencil_tic: 0,
        storage_tic: [0; 8],
        set_size: 0,
        entries: vec![PipelineEntry::Ubo {
            off: 0,
            stage: 2,
            slot: 1,
            ssbo: false,
            residual: None,
            window: 0,
        }],
        vb_slot: 0xFF,
        vb_stride: 0,
        vertex_streams: vec![],
        array_length_word: 0,
        viewport_count_word: 0,
        fetch: false,
        bound_words: vec![0],
        memory_words: vec![0],
        root_table_word: [0; 2],
        status_word: [0; 2],
        root_slots: [0; 2],
        heap: vec![],
        heap_bytes: 0,
        spill_entry: [0; 2],
        spill: [vec![], vec![]],
        uniform: {
            let mut u = [0xFFu8; 32];
            u[0] = 0;
            u[1] = 3;
            u
        },
        fetch_rung: [0; 8],
        written_format: [0; 16],
        variants: vec![],
    };
    assert_eq!(fragment_banks(&pipe), vec![(1, 3)]);
}
