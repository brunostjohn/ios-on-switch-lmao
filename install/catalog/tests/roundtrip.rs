// SPDX-License-Identifier: GPL-3.0-or-later
//! records.json, parsed, against ipsw/pins.bzl's IPSWS and staged.bzl's STAGED as JSON: every pin of every pinned kind
//! is in its record with the same value, and the record holds nothing else from them.

use serde_json::Value;
use switch_catalog::{Catalog, Member, Record, Role};

fn json(var: &str) -> Value {
    let path = std::env::var(var).unwrap_or_else(|_| panic!("{var} is set by the BUILD file"));
    serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap()
}

fn text(value: &Value) -> &str {
    value.as_str().unwrap_or_else(|| panic!("{value} is no string"))
}

fn number(value: &Value) -> u64 {
    value.as_u64().unwrap_or_else(|| panic!("{value} is no number"))
}

fn member_pin(kind: &str, role: &str, key: &str, pin: &Value, member: &Member) {
    let at = format!("{kind} {role} {key}");
    match key {
        "member" => assert_eq!(member.member, text(pin), "{at}"),
        "component" => assert_eq!(member.component, text(pin), "{at}"),
        "offset" => assert_eq!(member.offset, number(pin), "{at}"),
        "stored_size" => assert_eq!(member.stored_size, number(pin), "{at}"),
        "size" => assert_eq!(member.size, number(pin), "{at}"),
        "deflated" => assert_eq!(Some(member.deflated), pin.as_bool(), "{at}"),
        "sha256" => assert_eq!(member.sha256.to_string(), text(pin), "{at}"),
        "content_sha256" => assert_eq!(member.content_sha256.to_string(), text(pin), "{at}"),
        "manifest_digest" => assert_eq!(member.manifest_digest.to_string(), text(pin), "{at}"),
        "aea_key" => assert!(member.aea.is_some(), "{at}: encrypted member must set aea"),
        "aea_raw_size" => assert_eq!(member.aea.as_ref().unwrap().raw_size, number(pin), "{at}"),
        "container" => assert_eq!(member.container.as_ref().unwrap().format, text(pin), "{at}"),
        "container_size" => assert_eq!(member.container.as_ref().unwrap().size, number(pin), "{at}"),
        "container_sha256" => assert_eq!(member.container.as_ref().unwrap().sha256.to_string(), text(pin), "{at}"),
        "payload_size" => assert_eq!(member.im4p.as_ref().unwrap().payload_size, number(pin), "{at}"),
        "payload_sha256" => assert_eq!(
            member.im4p.as_ref().unwrap().payload_sha256.to_string(),
            text(pin),
            "{at}"
        ),
        _ => panic!("{at}: a pin the round trip doesn't know"),
    }
}

fn derived_pin(kind: &str, key: &str, pin: &Value, record: &Record) {
    let at = format!("{kind} derived {key}");
    let derived = &record.derived;
    match key {
        "kernel_virt_base" => assert_eq!(format!("{:#x}", derived.kernel_virt_base), text(pin), "{at}"),
        "kernelcache_sha256" => {
            let im4p = record.members["kernelcache"].im4p.as_ref().unwrap();
            assert_eq!(im4p.payload_sha256.to_string(), text(pin), "{at}");
        }
        "quartzcore_metallib_sha256" => assert_eq!(derived.quartzcore_metallib_sha256.to_string(), text(pin), "{at}"),
        "launchd_catalog_sha256" => assert_eq!(derived.launchd_catalog_sha256.unwrap().to_string(), text(pin), "{at}"),
        "launchd_catalog_signature_sha256" => {
            assert_eq!(
                derived.launchd_catalog_signature_sha256.unwrap().to_string(),
                text(pin),
                "{at}"
            );
        }
        "pretranslated" => {
            let pins = pin.as_object().unwrap();
            assert_eq!(derived.pretranslated.len(), pins.len(), "{at}");
            for (file, sha) in pins {
                assert_eq!(derived.pretranslated[file].to_string(), text(sha), "{at} {file}");
            }
        }
        _ => panic!("{at}: a pin the round trip doesn't know"),
    }
}

fn record_pins(kind: &str, pins: &Value, staged: &Value, record: &Record) -> usize {
    let mut checked = 0;
    for (key, pin) in pins.as_object().unwrap() {
        let at = format!("{kind} {key}");
        match key.as_str() {
            "build" => assert_eq!(record.build, text(pin), "{at}"),
            "version" => assert_eq!(record.version, text(pin), "{at}"),
            "ios" => assert_eq!(record.ios.to_string(), text(pin), "{at}"),
            "xnu" => assert_eq!(record.xnu, text(pin), "{at}"),
            "product" => assert_eq!(record.product, text(pin), "{at}"),
            "device_class" => assert_eq!(record.device_class, text(pin), "{at}"),
            "url" => assert_eq!(record.ipsw.url, text(pin), "{at}"),
            "size" => assert_eq!(record.ipsw.size, number(pin), "{at}"),
            "volumes" => {
                let volumes = serde_json::json!({
                    "os_volume_uuid": record.volumes.os_volume_uuid,
                    "system_cryptex_uuid": record.volumes.system_cryptex_uuid,
                    "system_cryptex_image": record.volumes.system_cryptex_image,
                    "app_cryptex_uuid": record.volumes.app_cryptex_uuid,
                    "app_cryptex_image": record.volumes.app_cryptex_image,
                });
                assert_eq!(&volumes, pin, "{at}");
            }
            "derived" => {
                for (key, pin) in pin.as_object().unwrap() {
                    derived_pin(kind, key, pin, record);
                    checked += 1;
                }
            }
            "members" => {
                let roles: Vec<&String> = pin.as_object().unwrap().keys().collect();
                assert_eq!(roles, record.members.keys().collect::<Vec<_>>(), "{at}: the roles");
                for (role, member_pins) in pin.as_object().unwrap() {
                    let member = &record.members[role];
                    let staged = staged.get(role).and_then(Value::as_object).cloned().unwrap_or_default();
                    for (key, pin) in member_pins.as_object().unwrap().iter().chain(&staged) {
                        member_pin(kind, role, key, pin, member);
                        checked += 1;
                    }
                }
            }
            _ => panic!("{at}: a pin the round trip doesn't know"),
        }
        checked += 1;
    }
    checked
}

#[test]
fn every_pin_is_in_its_record() {
    let raw_records = json("SWITCH_RECORDS");
    let raw_records_bytes = serde_json::to_vec(&raw_records).unwrap();
    let raw_records_text = std::str::from_utf8(&raw_records_bytes).unwrap();
    let catalog = Catalog::parse(&raw_records_bytes).unwrap();
    let ipsws = json("SWITCH_IPSWS");
    let staged = json("SWITCH_STAGED");
    for (_, pins) in ipsws.as_object().unwrap() {
        if let Some(members) = pins.get("members").and_then(Value::as_object) {
            for (_, member_pins) in members {
                if let Some(key) = member_pins.get("aea_key").and_then(Value::as_str) {
                    assert!(
                        !raw_records_text.contains(key),
                        "records.json must never include pinned AEA decryption keys"
                    );
                }
            }
        }
    }
    let pinned: Vec<&String> = ipsws
        .as_object()
        .unwrap()
        .iter()
        .filter(|(_, pins)| pins.get("members").is_some_and(|m| !m.as_object().unwrap().is_empty()))
        .map(|(kind, _)| kind)
        .collect();
    assert_eq!(
        catalog.builds.iter().map(|r| &r.build).collect::<Vec<_>>(),
        pinned,
        "one record per pinned kind: a kind missing here lacks pins catalog.bzl's catalog_kinds needs"
    );
    for record in &catalog.builds {
        let kind = &record.build;
        let checked = record_pins(kind, &ipsws[kind], &staged[kind], record);
        let build = record.plan().unwrap_or_else(|e| panic!("{e}"));
        for role in Role::ALL.into_iter().filter(|role| role.is_disk_image()) {
            let (_, pins) = build.container(role).unwrap();
            let member = build.member(role);
            if pins.format == "raw" {
                assert_eq!(
                    (pins.size, pins.sha256),
                    (member.content_size(), member.content_sha256),
                    "{kind} {role:?}"
                );
            }
        }
        assert!(record.assets.is_empty());
        println!("{kind}: {checked} pins equal, plans as {:?}", build.steps);
    }
}
