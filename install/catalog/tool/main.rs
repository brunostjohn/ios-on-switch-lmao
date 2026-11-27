// SPDX-License-Identifier: GPL-3.0-or-later
//! Writes catalog.json either from catalog.bzl's asset-free records plus `--asset` files, or by merging per-kind
//! catalogs (`--merge`). Refuses any catalog this installer cannot parse, plan, or validate against required roles
//! and optional `--assets-dir` files.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{BufReader, ErrorKind, Read};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::Parser;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use switch_catalog::{ASSET_ROLES, Catalog};

#[derive(Parser)]
struct Args {
    /// The catalog without assets, from catalog.bzl.
    #[arg(long, conflicts_with = "merge")]
    records: Option<PathBuf>,
    /// One asset of a build: the build, the asset's role, its name in the release, and the file.
    #[arg(long, num_args = 4, value_names = ["BUILD", "ROLE", "NAME", "FILE"], conflicts_with = "merge")]
    asset: Vec<String>,
    /// Existing per-kind catalog.json files to merge into one unified release catalog.
    #[arg(long, conflicts_with_all = ["records", "asset"])]
    merge: Vec<PathBuf>,
    /// Directory containing published asset files; verifies every referenced asset's size and SHA-256.
    #[arg(long)]
    assets_dir: Option<PathBuf>,
    /// Asset roles required on every build record (defaults to `boot`).
    #[arg(long)]
    require_role: Vec<String>,
    #[arg(long)]
    out: PathBuf,
}

fn hash(path: &Path) -> Result<(u64, String), String> {
    let at = |e: std::io::Error| format!("{}: {e}", path.display());
    let mut file = BufReader::with_capacity(1 << 20, File::open(path).map_err(at)?);
    let (mut hasher, mut len, mut buf) = (Sha256::new(), 0u64, vec![0; 1 << 20]);
    loop {
        match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                hasher.update(&buf[..n]);
                len += n as u64;
            }
            Err(e) if e.kind() == ErrorKind::Interrupted => {}
            Err(e) => return Err(at(e)),
        }
    }
    Ok((len, hex::encode(hasher.finalize())))
}

fn add_assets(catalog: &mut Value, assets: &[String]) -> Result<(), String> {
    let builds = catalog["builds"].as_array_mut().ok_or("the records have no builds")?;
    for asset in assets.chunks(4) {
        let [build, role, name, file] = asset else {
            unreachable!("clap gives --asset four values")
        };
        let record = builds
            .iter_mut()
            .find(|record| record["build"] == build.as_str())
            .ok_or_else(|| format!("an asset for {build}, which the records don't hold"))?;
        let (size, sha256) = hash(Path::new(file))?;
        let entry = json!({"name": name, "size": size, "sha256": sha256});
        if record["assets"]
            .as_object_mut()
            .ok_or("a record without assets")?
            .insert(role.clone(), entry)
            .is_some()
        {
            return Err(format!("two {role} assets for {build}"));
        }
    }
    Ok(())
}

fn merge_catalogs(paths: &[PathBuf]) -> Result<Value, String> {
    let mut expected_header: Option<(u32, String)> = None;
    let mut by_build: BTreeMap<String, Value> = BTreeMap::new();

    for path in paths {
        let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let parsed = Catalog::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        match &expected_header {
            None => expected_header = Some((parsed.format, parsed.release.clone())),
            Some((format, release)) => {
                if parsed.format != *format {
                    return Err(format!(
                        "{}: format {} does not match {}",
                        path.display(),
                        parsed.format,
                        format
                    ));
                }
                if parsed.release != *release {
                    return Err(format!(
                        "{}: release {:?} does not match {:?}",
                        path.display(),
                        parsed.release,
                        release
                    ));
                }
            }
        }
        let raw: Value = serde_json::from_slice(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        let builds = raw["builds"]
            .as_array()
            .ok_or_else(|| format!("{}: missing builds array", path.display()))?;
        for record in builds {
            let build = record["build"]
                .as_str()
                .ok_or_else(|| format!("{}: build entry has no string build id", path.display()))?;
            if by_build.insert(build.to_owned(), record.clone()).is_some() {
                return Err(format!("duplicate build {build} across merged catalogs"));
            }
        }
    }

    let (format, release) = expected_header.ok_or("no --merge catalogs provided")?;
    let builds: Vec<Value> = by_build.into_values().collect();
    Ok(json!({
        "format": format,
        "release": release,
        "builds": builds,
    }))
}

fn verify_assets_dir(catalog: &Catalog, dir: &Path) -> Result<(), String> {
    let mut checked = BTreeSet::new();
    for record in &catalog.builds {
        for (role, asset) in &record.assets {
            if !checked.insert((asset.name.clone(), asset.size, asset.sha256.0)) {
                continue;
            }
            let path = dir.join(&asset.name);
            let file =
                File::open(&path).map_err(|e| format!("{} {role} asset {}: {e}", record.build, path.display()))?;
            asset
                .check(BufReader::new(file))
                .map_err(|e| format!("{} {role} asset: {e}", record.build))?;
        }
    }
    Ok(())
}

fn run(args: &Args) -> Result<(), String> {
    let catalog = if !args.merge.is_empty() {
        merge_catalogs(&args.merge)?
    } else if let Some(records_path) = &args.records {
        let records = std::fs::read(records_path).map_err(|e| format!("{}: {e}", records_path.display()))?;
        let mut catalog: Value = serde_json::from_slice(&records).map_err(|e| e.to_string())?;
        add_assets(&mut catalog, &args.asset)?;
        catalog
    } else {
        return Err("pass either --records or --merge".into());
    };

    let mut json = serde_json::to_vec_pretty(&catalog).map_err(|e| e.to_string())?;
    json.push(b'\n');

    let parsed = Catalog::parse(&json).map_err(|e| e.to_string())?;
    let required: Vec<&str> = if args.require_role.is_empty() {
        ASSET_ROLES.to_vec()
    } else {
        args.require_role.iter().map(String::as_str).collect()
    };
    let mut by_name: BTreeMap<String, (u64, [u8; 32])> = BTreeMap::new();
    for record in &parsed.builds {
        let build = record.plan().map_err(|e| e.to_string())?;
        for role in &required {
            build.asset(role).map_err(|e| e.to_string())?;
        }
        for asset in record.assets.values() {
            let key = (asset.size, asset.sha256.0);
            if let Some(prev) = by_name.insert(asset.name.clone(), key)
                && prev != key
            {
                return Err(format!("conflicting size/sha256 for shared asset {}", asset.name));
            }
        }
    }
    if let Some(dir) = &args.assets_dir {
        verify_assets_dir(&parsed, dir)?;
    }
    std::fs::write(&args.out, json).map_err(|e| format!("{}: {e}", args.out.display()))
}

fn main() -> ExitCode {
    match run(&Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("switch_catalog_tool: {message}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use switch_catalog::Role;

    fn sample_member(role: Role) -> Value {
        let mut m = json!({
            "member": format!("Firmware/{}.bin", role.name()),
            "component": "C",
            "offset": 1,
            "stored_size": 2,
            "size": 3,
            "sha256": "11".repeat(32),
            "content_sha256": "22".repeat(32),
            "deflated": true,
            "manifest_digest": "33".repeat(48),
        });
        if role.is_disk_image() {
            m["container"] = json!({"format": "raw", "size": 3, "sha256": "22".repeat(32)});
        } else {
            m["im4p"] = json!({"fourcc": "abcd", "payload_size": 4, "payload_sha256": "44".repeat(32)});
        }
        m
    }

    fn sample_record(build: &str) -> Value {
        let members: serde_json::Map<String, Value> = Role::ALL
            .into_iter()
            .map(|r| (r.name().to_owned(), sample_member(r)))
            .collect();
        json!({
            "build": build,
            "version": "17.7",
            "ios": 17,
            "xnu": "10063.141.1",
            "product": "iPad7,1",
            "device_class": "j120ap",
            "ipsw": {"url": "https://example.invalid/a.ipsw", "size": 9},
            "steps": {"prelink": "native"},
            "members": members,
            "volumes": {
                "os_volume_uuid": "A",
                "system_cryptex_uuid": "B",
                "system_cryptex_image": "b.dmg",
                "app_cryptex_uuid": "C",
                "app_cryptex_image": "c.dmg"
            },
            "derived": {
                "kernel_virt_base": "0xfffffff007004000",
                "quartzcore_metallib_sha256": "55".repeat(32)
            },
            "assets": {},
        })
    }

    fn write_records(path: &Path, release: &str, builds: Vec<Value>) {
        let cat = json!({"format": 1, "release": release, "builds": builds});
        std::fs::write(path, serde_json::to_vec_pretty(&cat).unwrap()).unwrap();
    }

    #[test]
    fn build_and_merge_multi_asset_catalogs_with_assets_dir_check() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path();
        let assets = dir.join("dist");
        std::fs::create_dir_all(&assets).unwrap();

        let boot17 = assets.join("ios-on-switch-lmao-1.0.0-boot-21H461.tar");
        let ov17 = assets.join("ios-on-switch-lmao-1.0.0-overlay-21H461.tar");
        let boot27 = assets.join("ios-on-switch-lmao-1.0.0-boot-24B119.tar");
        let ov27 = assets.join("ios-on-switch-lmao-1.0.0-overlay-24B119.tar");
        let bootstrap = assets.join("ios-on-switch-lmao-1.0.0-bootstrap.tar");
        std::fs::write(&boot17, b"boot17-bytes").unwrap();
        std::fs::write(&ov17, b"overlay17-bytes").unwrap();
        std::fs::write(&boot27, b"boot27-bytes").unwrap();
        std::fs::write(&ov27, b"overlay27-bytes").unwrap();
        std::fs::write(&bootstrap, b"shared-bootstrap-bytes").unwrap();

        let rec17 = dir.join("rec17.json");
        let rec27 = dir.join("rec27.json");
        let cat17 = dir.join("cat17.json");
        let cat27 = dir.join("cat27.json");
        write_records(&rec17, "1.0.0", vec![sample_record("21H461")]);
        write_records(&rec27, "1.0.0", vec![sample_record("24B119")]);

        run(&Args {
            records: Some(rec17),
            asset: vec![
                "21H461".into(),
                "boot".into(),
                boot17.file_name().unwrap().to_str().unwrap().into(),
                boot17.to_str().unwrap().into(),
                "21H461".into(),
                "overlay".into(),
                ov17.file_name().unwrap().to_str().unwrap().into(),
                ov17.to_str().unwrap().into(),
                "21H461".into(),
                "bootstrap".into(),
                bootstrap.file_name().unwrap().to_str().unwrap().into(),
                bootstrap.to_str().unwrap().into(),
            ],
            merge: vec![],
            assets_dir: None,
            require_role: vec!["boot".into(), "overlay".into(), "bootstrap".into()],
            out: cat17.clone(),
        })
        .unwrap();

        run(&Args {
            records: Some(rec27),
            asset: vec![
                "24B119".into(),
                "boot".into(),
                boot27.file_name().unwrap().to_str().unwrap().into(),
                boot27.to_str().unwrap().into(),
                "24B119".into(),
                "overlay".into(),
                ov27.file_name().unwrap().to_str().unwrap().into(),
                ov27.to_str().unwrap().into(),
                "24B119".into(),
                "bootstrap".into(),
                bootstrap.file_name().unwrap().to_str().unwrap().into(),
                bootstrap.to_str().unwrap().into(),
            ],
            merge: vec![],
            assets_dir: None,
            require_role: vec!["boot".into(), "overlay".into(), "bootstrap".into()],
            out: cat27.clone(),
        })
        .unwrap();

        let merged = dir.join("catalog.json");
        run(&Args {
            records: None,
            asset: vec![],
            merge: vec![cat27.clone(), cat17.clone()],
            assets_dir: Some(assets.clone()),
            require_role: vec!["boot".into(), "overlay".into(), "bootstrap".into()],
            out: merged.clone(),
        })
        .unwrap();

        let parsed = Catalog::parse(&std::fs::read(&merged).unwrap()).unwrap();
        let build_ids: Vec<&str> = parsed.builds.iter().map(|b| b.build.as_str()).collect();
        assert_eq!(build_ids, vec!["21H461", "24B119"]);
        for rec in &parsed.builds {
            let build = rec.plan().unwrap();
            assert!(build.asset("boot").is_ok());
            assert!(build.asset("overlay").is_ok());
            assert!(build.asset("bootstrap").is_ok());
        }

        std::fs::write(&ov17, b"corrupted").unwrap();
        let err = run(&Args {
            records: None,
            asset: vec![],
            merge: vec![cat17.clone(), cat27.clone()],
            assets_dir: Some(assets),
            require_role: vec![],
            out: dir.join("bad.json"),
        })
        .unwrap_err();
        assert!(err.contains("overlay"), "{err}");
    }

    #[test]
    fn merge_refuses_duplicate_builds_and_mismatched_release() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path();
        let f = dir.join("boot.tar");
        std::fs::write(&f, b"boot").unwrap();

        let mk = |name: &str, release: &str, build: &str| {
            let rec = dir.join(format!("{name}.rec.json"));
            let out = dir.join(format!("{name}.cat.json"));
            write_records(&rec, release, vec![sample_record(build)]);
            run(&Args {
                records: Some(rec),
                asset: vec![
                    build.into(),
                    "boot".into(),
                    format!("boot-{build}.tar"),
                    f.to_str().unwrap().into(),
                ],
                merge: vec![],
                assets_dir: None,
                require_role: vec![],
                out: out.clone(),
            })
            .unwrap();
            out
        };

        let c1 = mk("a", "1.0.0", "21H461");
        let c1_dup = mk("b", "1.0.0", "21H461");
        let c2_rel = mk("c", "2.0.0", "24B119");

        let dup_err = run(&Args {
            records: None,
            asset: vec![],
            merge: vec![c1.clone(), c1_dup],
            assets_dir: None,
            require_role: vec![],
            out: dir.join("dup.json"),
        })
        .unwrap_err();
        assert!(dup_err.contains("duplicate build 21H461"), "{dup_err}");

        let rel_err = run(&Args {
            records: None,
            asset: vec![],
            merge: vec![c1, c2_rel],
            assets_dir: None,
            require_role: vec![],
            out: dir.join("rel.json"),
        })
        .unwrap_err();
        assert!(rel_err.contains("release"), "{rel_err}");
    }
}
