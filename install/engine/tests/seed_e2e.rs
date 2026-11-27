// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::path::{Path, PathBuf};

use switch_catalog::Catalog;
use switch_engine::{CancelToken, EngineError, EngineEvent, Mode, NoopSink, RunRequest, TargetSpec, run};
use switch_image_gpu::{REQUIRED_PLUGIN_PATH, TREE_MANIFEST};
use switch_seed::{
    EXPECTED_SPLIT_BUCKETS, MANIFEST_FORMAT, SeedAssetFile, SeedAssetManifest, SeedBucketCounts, SeedError,
};

use crate::assets_e2e::{append_tar_file, create_install_card};
use crate::synth_apfs::{sha256, sha256_file};
use crate::synth_fixture::{SyntheticFixture, build_synthetic_fixture};

fn write_valid_seed_tar(path: &Path, build: &str, stack: &str, qc_sha: &str) {
    let mut entries: Vec<(String, Vec<u8>)> = vec![
        ("raw/own.metallib".into(), b"MTLB-dummy".to_vec()),
        ("raw/context.log".into(), b"init\n".to_vec()),
        ("raw/draw_context.txt".into(), Vec::new()),
        ("raw/corpus.log".into(), Vec::new()),
        ("split/corpus-report.txt".into(), Vec::new()),
    ];
    for i in 0..EXPECTED_SPLIT_BUCKETS {
        entries.push((format!("split/bucket{i}.log"), Vec::new()));
    }
    let files = entries
        .iter()
        .map(|(p, data)| SeedAssetFile {
            path: p.clone(),
            sha256: hex::encode(sha256(data)),
            size: data.len() as u64,
        })
        .collect();
    let manifest = SeedAssetManifest {
        format: MANIFEST_FORMAT,
        build: build.to_owned(),
        kind: build.to_owned(),
        stack_key: stack.to_owned(),
        quartzcore_metallib_sha256: qc_sha.to_owned(),
        bucket_counts: SeedBucketCounts {
            lines: vec![0; EXPECTED_SPLIT_BUCKETS],
            split: EXPECTED_SPLIT_BUCKETS,
        },
        embedded: Vec::new(),
        app_libraries: Vec::new(),
        files,
    };
    let m_bytes = serde_json::to_vec_pretty(&manifest).unwrap();
    let file = File::create(path).unwrap();
    let mut tar = tar::Builder::new(file);
    append_tar_file(&mut tar, "manifest.json", &m_bytes, 0o644);
    for (p, data) in &entries {
        append_tar_file(&mut tar, p, data, 0o644);
    }
    tar.finish().unwrap();
}

fn write_gpu_tar(path: &Path, include_stack_key: bool) {
    let plugin_bytes = b"\xcf\xfa\xed\xfe-seed-test-plugin";
    let plist_rel = "ngpu/addons/TegraGM20BMetal.bundle/Info.plist";
    let plist_bytes: &[u8] = if include_stack_key {
        br#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>SwitchShaderStack</key><string>0123456789abcdef</string></dict></plist>"#
    } else {
        br#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>com.example.gpu</string></dict></plist>"#
    };
    let kext_digest = "ab".repeat(32);
    let manifest = format!(
        "# gpu stage 2 tree\n\
         # kext {kext_digest}\n\
         file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n\
         file\t{plist_rel}\t{}\t{}\t644\t{plist_rel}\n",
        hex::encode(sha256(plugin_bytes)),
        plugin_bytes.len(),
        hex::encode(sha256(plist_bytes)),
        plist_bytes.len(),
    );
    let file = File::create(path).unwrap();
    let mut tar = tar::Builder::new(file);
    append_tar_file(&mut tar, TREE_MANIFEST, manifest.as_bytes(), 0o644);
    append_tar_file(&mut tar, REQUIRED_PLUGIN_PATH, plugin_bytes, 0o755);
    append_tar_file(&mut tar, plist_rel, plist_bytes, 0o644);
    tar.finish().unwrap();
}

fn setup_seed_catalog(fixture: &SyntheticFixture, include_stack_key: bool) -> (PathBuf, PathBuf) {
    let assets_dir = fixture.tmp.path().join("seed_assets");
    fs::create_dir_all(&assets_dir).unwrap();
    let base_cat = Catalog::parse(&fs::read(&fixture.catalog_path).unwrap()).unwrap();
    let qc_sha = base_cat
        .record("24B119")
        .unwrap()
        .derived
        .quartzcore_metallib_sha256
        .to_string();

    let gpu_tar = assets_dir.join("ios-on-switch-lmao-test-gpu-24B119.tar");
    write_gpu_tar(&gpu_tar, include_stack_key);
    let seed_tar = assets_dir.join("ios-on-switch-lmao-test-seed-24B119.tar");
    write_valid_seed_tar(&seed_tar, "24B119", "0123456789abcdef", &qc_sha);

    let (gpu_len, gpu_sha) = sha256_file(&gpu_tar);
    let (seed_len, seed_sha) = sha256_file(&seed_tar);
    let mut catalog: serde_json::Value = serde_json::from_slice(&fs::read(&fixture.catalog_path).unwrap()).unwrap();
    let mut assets_map = serde_json::Map::new();
    assets_map.insert(
        "gpu".into(),
        serde_json::json!({ "name": gpu_tar.file_name().unwrap().to_str().unwrap(), "size": gpu_len, "sha256": gpu_sha }),
    );
    assets_map.insert(
        "seed".into(),
        serde_json::json!({ "name": seed_tar.file_name().unwrap().to_str().unwrap(), "size": seed_len, "sha256": seed_sha }),
    );
    catalog["builds"][0]["assets"] = serde_json::Value::Object(assets_map);
    let catalog_path = fixture.tmp.path().join("seed-catalog.json");
    let cat_bytes = serde_json::to_vec_pretty(&catalog).unwrap();
    Catalog::parse(&cat_bytes).unwrap();
    fs::write(&catalog_path, cat_bytes).unwrap();
    (catalog_path, assets_dir)
}

fn seed_run_request(fixture: &SyntheticFixture, img: &Path, catalog_path: &Path, assets_dir: &Path) -> RunRequest {
    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(img));
    req.build_id = Some("24B119".to_owned());
    req.work_dir = Some(fixture.work_dir.clone());
    req.log_path = Some(fixture.log_path.clone());
    req.local.catalog_path = Some(catalog_path.to_path_buf());
    req.local.assets_dir = Some(assets_dir.to_path_buf());
    req.local.os_dmg = Some(fixture.os_path.clone());
    req.local.system_cryptex = Some(fixture.sys_cryptex_path.clone());
    req.local.app_cryptex = Some(fixture.app_cryptex_path.clone());
    req.local.boot_files_dir = Some(fixture.boot_files_dir.clone());
    req
}

#[test]
fn seed_stage_maps_cancellation_to_engine_cancelled() {
    let fixture = build_synthetic_fixture(false);
    let (catalog_path, assets_dir) = setup_seed_catalog(&fixture, true);
    let img = fixture.tmp.path().join("seed-cancel.img");
    create_install_card(&img);
    let req = seed_run_request(&fixture, &img, &catalog_path, &assets_dir);

    let cancel = CancelToken::new();
    let cancel_in_sink = cancel.clone();
    let mut sink = move |event: EngineEvent| {
        if let EngineEvent::StepStarted { step_id, .. } = event
            && step_id == "seed"
        {
            cancel_in_sink.cancel();
        }
    };
    let err = run(&req, None, &mut sink, cancel).unwrap_err();
    assert!(
        matches!(err, EngineError::Cancelled),
        "expected EngineError::Cancelled when cancelled at seed start, got {err:?}"
    );
}

#[test]
fn seed_stage_rejects_gpu_bundle_missing_stack_key() {
    let fixture = build_synthetic_fixture(false);
    let (catalog_path, assets_dir) = setup_seed_catalog(&fixture, false);
    let img = fixture.tmp.path().join("seed-no-stack.img");
    create_install_card(&img);
    let req = seed_run_request(&fixture, &img, &catalog_path, &assets_dir);

    let mut sink = NoopSink;
    let err = run(&req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(&err, EngineError::Config(msg) if msg.contains("SwitchShaderStack")),
        "expected missing SwitchShaderStack Config error, got {err:?}"
    );
}

#[test]
fn seed_stage_unpacks_valid_seed_asset_and_runs_crawl() {
    let fixture = build_synthetic_fixture(false);
    let (catalog_path, assets_dir) = setup_seed_catalog(&fixture, true);
    let img = fixture.tmp.path().join("seed-valid-manifest.img");
    create_install_card(&img);
    let req = seed_run_request(&fixture, &img, &catalog_path, &assets_dir);

    let mut sink = NoopSink;
    let err = run(&req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(&err, EngineError::Seed(SeedError::Asset(msg)) if msg.contains("QuartzCore")),
        "expected valid seed tar to pass manifest validation and reach crawl QuartzCore check, got {err:?}"
    );
}
