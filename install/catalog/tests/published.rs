// SPDX-License-Identifier: GPL-3.0-or-later
//! The catalog the release publishes, with the files published beside it: every record plans, every asset is the
//! file of its name, and without its assets the catalog is records.json.

use std::fs::File;
use std::io::BufReader;
use std::path::{Path, PathBuf};

use serde_json::Value;
use switch_catalog::{ASSET_ROLES, CATALOG_NAME, Catalog};

fn files() -> Vec<PathBuf> {
    let listed = std::env::var("SWITCH_PUBLISHED").expect("SWITCH_PUBLISHED is set by the BUILD file");
    listed.split_whitespace().map(PathBuf::from).collect()
}

fn named<'a>(files: &'a [PathBuf], name: &str) -> &'a Path {
    let mut found = files.iter().filter(|f| f.file_name().is_some_and(|n| n == name));
    match (found.next(), found.next()) {
        (Some(file), None) => file,
        _ => panic!("not one published file is {name}"),
    }
}

#[test]
fn every_asset_is_the_file_published_under_its_name() {
    let files = files();
    let json = std::fs::read(named(&files, CATALOG_NAME)).unwrap();
    let catalog = Catalog::parse(&json).unwrap();
    let mut names = vec![CATALOG_NAME.to_owned()];
    for record in &catalog.builds {
        let build = record.plan().unwrap_or_else(|e| panic!("{e}"));
        for role in ASSET_ROLES {
            let asset = build.asset(role).unwrap();
            let file = named(&files, &asset.name);
            asset.check(BufReader::new(File::open(file).unwrap())).unwrap();
            println!(
                "{} {role}: {} {} B, sha256 {}",
                record.build, asset.name, asset.size, asset.sha256
            );
            names.push(asset.name.clone());
        }
    }
    let mut published: Vec<String> = files
        .iter()
        .map(|f| f.file_name().unwrap().to_str().unwrap().to_owned())
        .collect();
    published.sort();
    names.sort();
    assert_eq!(published, names, "the published files are the catalog and its assets");

    let mut stripped: Value = serde_json::from_slice(&json).unwrap();
    for record in stripped["builds"].as_array_mut().unwrap() {
        record["assets"] = serde_json::json!({});
    }
    let records_path = std::env::var("SWITCH_RECORDS").unwrap();
    let records: Value = serde_json::from_slice(&std::fs::read(records_path).unwrap()).unwrap();
    assert_eq!(stripped, records, "the catalog without its assets is records.json");
}
