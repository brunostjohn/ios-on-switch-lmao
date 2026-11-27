// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected kind's trust bundle, kernel window, constants.json and bl33.bin made here from //boot:package's own
//! inputs (its prelinked kernel and ADT, the overlay module without installd and installd's CDHash): each the same
//! bytes as the package's, and the layout the same as its plan at every step on the way.

mod case;

use std::time::Instant;

use case::{BootFiles, Case, slices};
use serde_json::Value;
use sha2::{Digest, Sha256};
use switch_trustcache::{CDHASH_LEN, module_entries};
use switch_window::{bl33, compress, join, layout, trust_bundle};

fn holds(module: &[u8], cdhash: &[u8; CDHASH_LEN]) -> bool {
    module_entries(module)
        .unwrap()
        .iter()
        .any(|entry| entry[..CDHASH_LEN] == *cdhash)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn the_packages_trust_window_constants_and_bl33() {
    let case = Case::load();
    let kind = &case.kind;
    let installd = case.installd();
    assert!(
        holds(&case.file("overlay_trust"), &installd),
        "{kind}: the package's overlay module lacks installd"
    );
    assert!(
        !holds(&case.file("overlay_trust_base"), &installd),
        "{kind}: the release's overlay module holds installd's CDHash"
    );

    let start = Instant::now();
    let modules = slices(&case.modules);
    let static_module = case.file("static_trust");
    let bundle = trust_bundle(&case.bundle(&static_module, &modules)).unwrap();
    assert!(
        bundle == case.file("trust"),
        "{kind}: the trust bundle differs from //boot:package's"
    );
    let trust_time = start.elapsed();

    let (kernel, adt, seed) = (case.file("kernel"), case.file("adt"), case.file("data_seed"));
    let start = Instant::now();
    let laid = layout(&case.inputs(&kernel, &adt, &bundle, &seed)).unwrap();
    let layout_time = start.elapsed();
    let plan: Value = serde_json::from_slice(&case.file("plan")).unwrap();
    assert_eq!(
        laid.image.len() as u64,
        plan["image"]["bytes"].as_u64().unwrap(),
        "{kind}: image size"
    );
    assert_eq!(
        hex(&Sha256::digest(&laid.image)),
        plan["image"]["sha256"].as_str().unwrap(),
        "{kind}: image"
    );
    assert_eq!(laid.entry_pa, plan["entry_pa"].as_u64().unwrap(), "{kind}: entry");
    let blocks: Vec<(usize, usize)> = serde_json::from_value(plan["blocks"].clone()).unwrap();
    assert!(laid.blocks == blocks, "{kind}: the LZ4 blocks differ from the plan's");
    let mut constants: Value = serde_json::from_str(&laid.constants.json()).unwrap();
    constants.as_object_mut().unwrap().remove("BLOCKS");
    assert_eq!(
        constants, plan["constants"],
        "{kind}: the layout's constants differ from the plan's"
    );

    let start = Instant::now();
    let compressed = compress(&laid).unwrap();
    let lz4_time = start.elapsed();
    let start = Instant::now();
    let staged = join(&laid, &compressed).unwrap();
    let join_time = start.elapsed();
    assert!(
        staged.staged == case.file("window"),
        "{kind}: kernel-window.bin differs from //boot:package's"
    );
    assert!(
        staged.constants.json().as_bytes() == case.file("constants"),
        "{kind}: constants.json differs from //boot:package's"
    );

    let files = BootFiles::of(&case);
    let start = Instant::now();
    let image = bl33(&staged, &files.boot(&files.constants)).unwrap();
    let stage_time = start.elapsed();
    assert!(
        image == case.file("bl33"),
        "{kind}: bl33.bin differs from //boot:package's"
    );
    println!(
        "{kind}: trust bundle {} B, image {} B in {} blocks, staged {} B, bl33.bin {} B, STAGE_ID {:#x}: all equal to \
         //boot:package's",
        bundle.len(),
        laid.image.len(),
        laid.blocks.len(),
        staged.staged.len(),
        image.len(),
        staged.constants.stage_id().unwrap()
    );
    let window_time = layout_time + lz4_time + join_time;
    println!(
        "{kind}: {} threads; trust {:.3} s, layout and checks {:.3} s, LZ4 {:.3} s, join {:.3} s, the whole window \
         {:.3} s, bl33 {:.3} s",
        rayon::current_num_threads(),
        trust_time.as_secs_f64(),
        layout_time.as_secs_f64(),
        lz4_time.as_secs_f64(),
        join_time.as_secs_f64(),
        window_time.as_secs_f64(),
        stage_time.as_secs_f64()
    );
}
