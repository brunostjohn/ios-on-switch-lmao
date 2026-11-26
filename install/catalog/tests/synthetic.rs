// SPDX-License-Identifier: GPL-3.0-or-later
//! A build no installer has seen: an iOS 18 record beside the generated ones, its every pin new and its steps ones
//! this installer knows. It plans with no change here; a step this installer doesn't know refuses that record alone.

use serde_json::{Value, json};
use switch_catalog::{Catalog, Container, Prelink, Role};

const BUILD: &str = "22H124";

fn records() -> Value {
    let path = std::env::var("SWITCH_RECORDS").expect("SWITCH_RECORDS is set by the BUILD file");
    serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
}

/// Every hash and number changed, so nothing of the build it was made from survives.
fn renew(value: &mut Value, seed: &mut u64) {
    match value {
        Value::Object(map) => map.values_mut().for_each(|v| renew(v, seed)),
        Value::Number(n) => *n = (n.as_u64().unwrap() + 1 + *seed).into(),
        Value::String(s) if s.len() == 64 || s.len() == 96 => {
            *seed += 1;
            *s = format!("{:02x}", *seed % 256).repeat(s.len() / 2);
        }
        _ => {}
    }
}

fn ios18(template: &Value) -> Value {
    let mut record = template.clone();
    let mut seed = 0;
    renew(&mut record["members"], &mut seed);
    renew(&mut record["derived"], &mut seed);
    record["build"] = json!(BUILD);
    record["version"] = json!("18.7.2");
    record["ios"] = json!(18);
    record["xnu"] = json!("11417.140.69");
    record["product"] = json!("iPad11,1");
    record["device_class"] = json!("j210ap");
    record["ipsw"] =
        json!({"url": "https://updates.cdn-apple.com/2025/iPad_18.7.2_22H124_Restore.ipsw", "size": 9_876_543_210u64});
    record["derived"]["kernel_virt_base"] = json!("0xfffffff007104000");
    record["members"]["os"]["aea"]["key"] = json!(format!("base64:{}", "A".repeat(43) + "="));
    record["members"]["os"]["container"]["format"] = json!("udif");
    record["volumes"]["os_volume_uuid"] = json!("00000000-1111-2222-3333-444444444444");
    record["assets"] = json!({"boot": {"name": format!("boot-{BUILD}.tar"), "size": 1, "sha256": "ab".repeat(32)}});
    record
}

fn catalog_with(new: Value) -> Catalog {
    let mut catalog = records();
    let template = catalog["builds"].as_array().unwrap().last().unwrap().clone();
    let mut record = ios18(&template);
    for (key, value) in new.as_object().unwrap() {
        record["steps"][key] = value.clone();
    }
    catalog["builds"].as_array_mut().unwrap().push(record);
    Catalog::parse(&serde_json::to_vec(&catalog).unwrap()).unwrap()
}

#[test]
fn a_new_build_with_known_steps_plans() {
    let catalog = catalog_with(json!({"prelink": "fileset"}));
    let record = catalog.record(BUILD).unwrap();
    let build = record.plan().unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(build.record.ios, 18);
    assert_eq!(build.steps.prelink, Prelink::Fileset);
    assert_eq!(build.record.derived.kernel_virt_base, 0xffff_fff0_0710_4000);
    assert_eq!(build.container(Role::Os).unwrap().0, Container::Udif);
    for role in Role::ALL {
        let (old, new) = (
            catalog.builds[catalog.builds.len() - 2].members[role.name()].clone(),
            build.member(role),
        );
        assert_ne!(old.content_sha256, new.content_sha256, "{role:?}");
        assert_ne!(old.offset, new.offset, "{role:?}");
    }
    assert_eq!(build.asset("boot").unwrap().name, format!("boot-{BUILD}.tar"));
    for record in &catalog.builds {
        println!("{}: {:?}", record.build, record.plan().unwrap().steps);
    }
}

#[test]
fn a_new_build_with_an_unknown_step_leaves_the_others_usable() {
    let catalog = catalog_with(json!({"prelink": "fileset2", "seed": "metal4"}));
    let mut usable = Vec::new();
    for record in &catalog.builds {
        match record.plan() {
            Ok(build) => usable.push(build.build().to_owned()),
            Err(refused) => {
                assert_eq!(refused.build, BUILD);
                assert_eq!(refused.reasons.len(), 2, "{refused}");
                println!("{refused}");
            }
        }
    }
    assert_eq!(usable.len(), catalog.builds.len() - 1);
}
