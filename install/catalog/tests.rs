// SPDX-License-Identifier: GPL-3.0-or-later
use serde_json::{Value, json};
use sha2::{Digest as _, Sha256};

use crate::{Catalog, Container, Error, Prelink, Role, Unsupported, catalog_url};

fn member(role: Role) -> Value {
    let mut member = json!({
        "member": format!("Firmware/{}.bin", role.name()), "component": "C", "offset": 1, "stored_size": 2,
        "size": 3, "sha256": "11".repeat(32), "content_sha256": "22".repeat(32), "deflated": true,
        "manifest_digest": "33".repeat(48),
    });
    if role.is_disk_image() {
        member["container"] = json!({"format": "raw", "size": 3, "sha256": "22".repeat(32)});
    } else {
        member["im4p"] = json!({"fourcc": "abcd", "payload_size": 4, "payload_sha256": "44".repeat(32)});
    }
    member
}

fn record(build: &str) -> Value {
    let members: serde_json::Map<String, Value> = Role::ALL
        .into_iter()
        .map(|role| (role.name().to_owned(), member(role)))
        .collect();
    json!({
        "build": build, "version": "1.0", "ios": 1, "xnu": "1.2.3", "product": "iPad1,1", "device_class": "j1ap",
        "ipsw": {"url": "https://example.invalid/a.ipsw", "size": 9},
        "steps": {"prelink": "native"},
        "members": members,
        "volumes": {"os_volume_uuid": "A", "system_cryptex_uuid": "B", "system_cryptex_image": "b.dmg",
                    "app_cryptex_uuid": "C", "app_cryptex_image": "c.dmg"},
        "derived": {"kernel_virt_base": "0xfffffff007004000", "quartzcore_metallib_sha256": "55".repeat(32)},
        "assets": {"boot": {"name": format!("boot-{build}.tar"), "size": 5, "sha256": hex::encode(Sha256::digest(b"tar!!"))}},
    })
}

fn catalog(records: Vec<Value>) -> Value {
    json!({"format": 1, "release": "0.1.0", "builds": records})
}

fn parse(value: &Value) -> Result<Catalog, Error> {
    Catalog::parse(&serde_json::to_vec(value).unwrap())
}

fn unsupported(record: Value) -> Unsupported {
    parse(&catalog(vec![record])).unwrap().builds[0].plan().unwrap_err()
}

#[test]
fn a_record_plans() {
    let catalog = parse(&catalog(vec![record("1A1")])).unwrap();
    let build = catalog.builds[0].plan().unwrap();
    assert_eq!(build.steps.prelink, Prelink::Native);
    assert!(!build.steps.launchd_catalog && !build.steps.pretranslate && !build.steps.boot_manifest_hash);
    assert_eq!(build.record.derived.kernel_virt_base, 0xffff_fff0_0700_4000);
    assert_eq!(build.container(Role::Os).unwrap().0, Container::Apfs);
    assert_eq!(build.member(Role::DeviceTree).file_name(), "device_tree.bin");
    assert_eq!(build.asset("boot").unwrap().size, 5);
}

#[test]
fn another_format_is_refused_before_its_fields_are_read() {
    let mut value = catalog(vec![record("1A1")]);
    value["format"] = json!(2);
    value["builds"] = json!("whatever format 2 holds");
    assert!(matches!(parse(&value), Err(Error::Format(2))));
}

#[test]
fn an_unknown_field_refuses_the_catalog() {
    let mut value = catalog(vec![record("1A1")]);
    value["builds"][0]["members"]["os"]["sparse"] = json!(true);
    let error = parse(&value).unwrap_err();
    assert!(matches!(&error, Error::Json(m) if m.contains("sparse")), "{error}");

    let mut value = catalog(vec![record("1A1")]);
    value["signature"] = json!("x");
    assert!(matches!(parse(&value), Err(Error::Json(_))));

    let mut value = catalog(vec![record("1A1")]);
    value["builds"][0]["members"]["os"]["aea"] = json!({"raw_size": 3, "key": "base64:AAAA"});
    let error = parse(&value).unwrap_err();
    assert!(matches!(&error, Error::Json(m) if m.contains("key")), "{error}");
}

#[test]
fn a_build_listed_twice_or_a_bad_hash_refuses_the_catalog() {
    assert!(matches!(
        parse(&catalog(vec![record("1A1"), record("1A1")])),
        Err(Error::Duplicate(b)) if b == "1A1"
    ));
    let mut value = record("1A1");
    value["members"]["os"]["sha256"] = json!("11".repeat(31));
    assert!(matches!(parse(&catalog(vec![value])), Err(Error::Json(m)) if m.contains("32 bytes")));
}

#[test]
fn an_unknown_step_kind_refuses_that_record_alone() {
    let mut new = record("2B2");
    new["steps"]["prelink"] = json!("fileset2");
    let catalog = parse(&catalog(vec![record("1A1"), new])).unwrap();
    let plans: Vec<_> = catalog.builds.iter().map(|record| record.plan()).collect();
    assert_eq!(plans[0].as_ref().unwrap().build(), "1A1");
    let refused = plans[1].as_ref().unwrap_err();
    assert_eq!(
        refused.to_string(),
        "this installer can't install 2B2: the prelink step's kind fileset2, which a newer installer may run"
    );
}

#[test]
fn unknown_steps_roles_containers_and_assets_are_refused() {
    let mut value = record("1A1");
    value["steps"]["seed"] = json!("v2");
    value["members"]["exclave"] = member(Role::DeviceTree);
    value["members"]["os"]["container"]["format"] = json!("sparse");
    value["assets"]["firmware"] = value["assets"]["boot"].clone();
    let reasons = unsupported(value).reasons;
    for expected in [
        "the seed step, which",
        "the exclave member, which",
        "the os member's container format sparse, which",
        "the firmware asset, which",
    ] {
        assert!(reasons.iter().any(|r| r.contains(expected)), "{expected}: {reasons:?}");
    }
}

#[test]
fn known_asset_roles_plan() {
    let mut value = record("1A1");
    let boot = value["assets"]["boot"].clone();
    for role in crate::KNOWN_ASSET_ROLES {
        value["assets"][role] = boot.clone();
    }
    let catalog = parse(&catalog(vec![value])).unwrap();
    let build = catalog.builds[0].plan().unwrap();
    for role in crate::KNOWN_ASSET_ROLES {
        assert_eq!(build.asset(role).unwrap().size, 5, "{role}");
    }
}

#[test]
fn pins_that_dont_fit_their_steps_or_roles_are_refused() {
    let mut value = record("1A1");
    value["steps"]["launchd_catalog"] = json!("jobs");
    value["derived"]["pretranslated"] = json!({"os.trustcache": "66".repeat(32)});
    value["members"].as_object_mut().unwrap().remove("app_cryptex");
    value["members"]["kernelcache"]["container"] = value["members"]["os"]["container"].clone();
    value["steps"].as_object_mut().unwrap().remove("prelink");
    let reasons = unsupported(value).reasons;
    for expected in [
        "launchd_catalog step",
        "pretranslate step",
        "no app_cryptex member",
        "kernelcache member is no disk image",
        "no prelink step",
    ] {
        assert!(reasons.iter().any(|r| r.contains(expected)), "{expected}: {reasons:?}");
    }
}

#[test]
fn an_asset_is_checked_to_its_end() {
    let catalog = parse(&catalog(vec![record("1A1")])).unwrap();
    let asset = &catalog.builds[0].assets["boot"];
    asset.check(&b"tar!!"[..]).unwrap();
    for wrong in [&b"tar!?"[..], b"tar!", b"tar!!!"] {
        assert!(matches!(asset.check(wrong), Err(Error::AssetMismatch { .. })));
    }
    let mut checked = asset.checked(&b"tar!!"[..]);
    let mut first = [0; 2];
    std::io::Read::read_exact(&mut checked, &mut first).unwrap();
    checked.finish().unwrap();
}

#[test]
fn urls_name_the_release() {
    let catalog = parse(&catalog(vec![record("1A1")])).unwrap();
    let repo = crate::REPO;
    assert_eq!(
        catalog.assets_url(),
        format!("https://github.com/{repo}/releases/download/0.1.0/")
    );
    assert_eq!(
        catalog_url(Some("0.1.0")),
        format!("https://github.com/{repo}/releases/download/0.1.0/catalog.json")
    );
    assert_eq!(
        catalog_url(None),
        format!("https://github.com/{repo}/releases/latest/download/catalog.json")
    );
}
