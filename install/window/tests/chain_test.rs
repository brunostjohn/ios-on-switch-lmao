// SPDX-License-Identifier: GPL-3.0-or-later
//! The installer's whole path to bl33.bin for the selected kind: the release's base ADT (//boot:adt_base) finished
//! with Apple's device tree (switch_adt), the trust bundle, the window, then bl33.bin, equal to //boot:package's.

mod case;

use case::{BootFiles, Case, read, slices};
use serde_json::Value;
use switch_adt::{Finish, TrustModules, finish};
use switch_window::{bl33, layout, trust_bundle, window};

fn adt_case() -> Value {
    let path = std::env::var("SWITCH_ADT_CASE").expect("SWITCH_ADT_CASE is not set");
    serde_json::from_slice(&read(&path)).unwrap()
}

fn files(case: &Value, key: &str) -> Vec<Vec<u8>> {
    case[key]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| read(p.as_str().unwrap()))
        .collect()
}

fn finished_adt(case: &Value) -> Vec<u8> {
    let optional = |key: &str| case[key].as_str().map(read);
    let launchd: Option<[u8; 64]> = optional("launchd_catalog_sha256").map(|d| d.try_into().expect("two digests"));
    let cache = optional("a64x_cache_pretranslated");
    let updates = optional("pretranslated_updates");
    let (os, app, pretranslated) = (
        files(case, "os_cryptex"),
        files(case, "app_cryptex"),
        files(case, "pretranslated"),
    );
    let (os, app, pretranslated) = (slices(&os), slices(&app), slices(&pretranslated));
    let given = Finish {
        root_volume_uuid: case["root_volume_uuid"].as_str().unwrap(),
        boot_manifest_hash: case["boot_manifest_hash"].as_bool().unwrap(),
        launchd_catalog_sha256: launchd.as_ref(),
        a64x_cache_pretranslated: cache.as_deref(),
        pretranslated_updates: updates.as_deref(),
        trust_modules: TrustModules {
            os_cryptex: &os,
            app_cryptex: &app,
            pretranslated: &pretranslated,
        },
    };
    let base = read(case["base"].as_str().unwrap());
    finish(&base, &read(case["apple"].as_str().unwrap()), &given).unwrap()
}

#[test]
fn from_the_release_base_adt_to_bl33() {
    let case = Case::load();
    let kind = &case.kind;
    let adt = finished_adt(&adt_case());
    assert!(
        adt == case.file("adt"),
        "{kind}: the finished ADT differs from //boot:package's"
    );

    let modules = slices(&case.modules);
    let static_module = case.file("static_trust");
    let trust = trust_bundle(&case.bundle(&static_module, &modules)).unwrap();
    let (kernel, seed) = (case.file("kernel"), case.file("data_seed"));
    let staged = window(&layout(&case.inputs(&kernel, &adt, &trust, &seed)).unwrap()).unwrap();
    let files = BootFiles::of(&case);
    let image = bl33(&staged, &files.boot(&files.constants)).unwrap();
    assert!(
        image == case.file("bl33"),
        "{kind}: bl33.bin differs from //boot:package's"
    );
    println!(
        "{kind}: //boot:adt_base, finish, trust bundle, window, bl33.bin ({} B): equal to //boot:package's",
        image.len()
    );
}
