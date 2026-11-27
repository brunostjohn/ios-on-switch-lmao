// SPDX-License-Identifier: GPL-3.0-or-later
//! bl33.bin is refused for a window the release's loader was not built for: one whose constants.json differs from the
//! loader's, key by key, and the window of a bundle that lacks installd's CDHash (whose STAGE_ID, the SHA-256 of the
//! staged stream, cannot match).

mod case;

use case::{BootFiles, Case, slices};
use serde_json::Value;
use switch_trustcache::{bundle, merged_module};
use switch_window::{Error, Window, bl33, layout, trust_bundle, window};

fn window_of(case: &Case, trust: &[u8]) -> Window {
    let (kernel, adt, seed) = (case.file("kernel"), case.file("adt"), case.file("data_seed"));
    window(&layout(&case.inputs(&kernel, &adt, trust, &seed)).unwrap()).unwrap()
}

fn refused_keys(result: Result<Vec<u8>, Error>) -> Vec<String> {
    match result {
        Err(Error::LoaderBuiltForAnotherWindow(keys)) => keys,
        Err(e) => panic!("refused for another reason: {e}"),
        Ok(_) => panic!("bl33.bin was made for a window the loader was not built for"),
    }
}

#[test]
fn constants_that_differ_from_the_loaders_are_refused_by_name() {
    let case = Case::load();
    let modules = slices(&case.modules);
    let static_module = case.file("static_trust");
    let staged = window_of(&case, &trust_bundle(&case.bundle(&static_module, &modules)).unwrap());
    let files = BootFiles::of(&case);
    assert!(
        bl33(&staged, &files.boot(&files.constants)).is_ok(),
        "{}: the package's own constants",
        case.kind
    );

    let asset: Value = serde_json::from_slice(&files.constants).unwrap();
    for (key, changed) in [
        ("EMC_KHZ", Value::from(1_331_200u64)),
        ("BLOCKS", Value::Array(Vec::new())),
    ] {
        let mut other = asset.clone();
        other[key] = changed;
        let other = serde_json::to_vec(&other).unwrap();
        assert_eq!(refused_keys(bl33(&staged, &files.boot(&other))), [key], "{}", case.kind);
    }
    let mut extra = asset.clone();
    extra["SPARE"] = Value::from(0);
    let extra = serde_json::to_vec(&extra).unwrap();
    assert_eq!(
        refused_keys(bl33(&staged, &files.boot(&extra))),
        ["SPARE"],
        "{}",
        case.kind
    );
}

#[test]
fn a_bundle_without_installds_cdhash_is_refused() {
    let case = Case::load();
    let static_module = case.file("static_trust");
    let mut modules = vec![static_module.as_slice()];
    modules.extend(case.modules.iter().map(Vec::as_slice).filter(|m| !m.is_empty()));
    let without = bundle(&merged_module(&modules).unwrap()).unwrap();
    assert!(
        without != case.file("trust"),
        "{}: installd's CDHash changed nothing",
        case.kind
    );

    let staged = window_of(&case, &without);
    let files = BootFiles::of(&case);
    let keys = refused_keys(bl33(&staged, &files.boot(&files.constants)));
    assert!(
        keys.iter().any(|k| k == "STAGE_ID"),
        "{}: refused for {keys:?} but not STAGE_ID",
        case.kind
    );
    println!(
        "{}: a bundle without installd's CDHash: refused, {} differ",
        case.kind,
        keys.join(", ")
    );
}
