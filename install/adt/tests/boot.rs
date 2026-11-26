// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected IPSW kind's //boot:package ADT, finished here from //boot:adt_base: the bytes must be the same.

use serde_json::Value;
use switch_adt::{Finish, TrustModules, finish};

fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn case() -> Value {
    let path = std::env::var("SWITCH_ADT_CASE").expect("SWITCH_ADT_CASE is not set");
    serde_json::from_slice(&read(&path)).unwrap()
}

fn optional(case: &Value, key: &str) -> Option<Vec<u8>> {
    case[key].as_str().map(read)
}

fn modules(case: &Value, role: &str) -> Vec<Vec<u8>> {
    case[role]
        .as_array()
        .unwrap()
        .iter()
        .map(|path| read(path.as_str().unwrap()))
        .collect()
}

fn slices(modules: &[Vec<u8>]) -> Vec<&[u8]> {
    modules.iter().map(Vec::as_slice).collect()
}

#[test]
fn finish_of_the_base_is_the_boot_packages_adt() {
    let case = case();
    let kind = case["kind"].as_str().unwrap();
    let launchd: Option<[u8; 64]> =
        optional(&case, "launchd_catalog_sha256").map(|d| d.try_into().expect("two SHA-256 digests"));
    let cache = optional(&case, "a64x_cache_pretranslated");
    let (os_cryptex, app_cryptex, pretranslated) = (
        modules(&case, "os_cryptex"),
        modules(&case, "app_cryptex"),
        modules(&case, "pretranslated"),
    );
    let (os_cryptex, app_cryptex, pretranslated) = (slices(&os_cryptex), slices(&app_cryptex), slices(&pretranslated));
    let given = Finish {
        root_volume_uuid: case["root_volume_uuid"].as_str().unwrap(),
        boot_manifest_hash: case["boot_manifest_hash"].as_bool().unwrap(),
        launchd_catalog_sha256: launchd.as_ref(),
        a64x_cache_pretranslated: cache.as_deref(),
        trust_modules: TrustModules {
            os_cryptex: &os_cryptex,
            app_cryptex: &app_cryptex,
            pretranslated: &pretranslated,
        },
    };
    let base = read(case["base"].as_str().unwrap());
    let got = finish(&base, &read(case["apple"].as_str().unwrap()), &given).unwrap();
    let oracle = read(case["adt"].as_str().unwrap());
    assert!(
        got == oracle,
        "{kind}: {} bytes differ from //boot:package's {} bytes",
        got.len(),
        oracle.len()
    );
    println!(
        "{kind}: base {} bytes, finished {} bytes, the same as //boot:package's (roles {}/{}/{} modules, launchd {}, \
         cache list {}, manifest hash slot {})",
        base.len(),
        got.len(),
        os_cryptex.len(),
        app_cryptex.len(),
        pretranslated.len(),
        launchd.is_some(),
        cache.is_some(),
        given.boot_manifest_hash
    );
}

fn uuid_bytes(uuid: &str) -> Vec<u8> {
    let digits: String = uuid.chars().filter(|c| *c != '-').collect();
    (0..digits.len())
        .step_by(2)
        .map(|at| u8::from_str_radix(&digits[at..at + 2], 16).unwrap())
        .collect()
}

#[test]
fn the_base_holds_no_pinned_system_volume() {
    let case = case();
    let base = read(case["base"].as_str().unwrap());
    let holds = |needle: &[u8]| base.windows(needle.len()).any(|w| w == needle);
    for uuid in case["pinned_volume_uuids"].as_array().unwrap() {
        let uuid = uuid.as_str().unwrap();
        for form in [
            uuid.to_ascii_uppercase().into_bytes(),
            uuid.to_ascii_lowercase().into_bytes(),
            uuid_bytes(uuid),
        ] {
            assert!(!holds(&form), "{}: the base holds {uuid}", case["kind"]);
        }
    }
}
