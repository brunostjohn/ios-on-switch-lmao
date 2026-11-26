// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::io::{Cursor, Read};
use std::path::{Path, PathBuf};

use flate2::read::GzDecoder;
use plist::Value as PlistValue;
use switch_apfs_format::io::FileSource;
use switch_apfs_read::{Container, Fs, stream};
use switch_card::{BASIC_DATA_TYPE, LINUX_TYPE, SECTOR, SETUP_NAME};
use switch_catalog::Catalog;
use switch_engine::boot::read_pretranslated_edits_from_boot_tar;
use switch_engine::progress::EngineProgress;
use switch_engine::{
    CancelToken, DATA_VOLUME_UUID_STR, EngineError, Mode, NoopSink, RunRequest, TargetSpec, resolve_release_asset, run,
};
use switch_image::parse_uuid;
use switch_image_bootstrap::HELD;
use switch_image_gpu::{GpuStage, ICON_CACHE_RECORD_PATH, REQUIRED_PLUGIN_PATH, STATE_PATH, TREE_MANIFEST};

use crate::synth_apfs::{sha256, sha256_file};
use crate::synth_fat::{FAT_FIRST_LBA, create_gpt_card_file, format_fat32_region, make_gpt_entry};
use crate::synth_fixture::{SyntheticFixture, build_synthetic_fixture};

fn append_tar_file<W: std::io::Write>(tar: &mut tar::Builder<W>, path: &str, data: &[u8], mode: u32) {
    let mut header = tar::Header::new_ustar();
    header.set_size(data.len() as u64);
    header.set_mode(mode);
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_cksum();
    tar.append_data(&mut header, path, data).unwrap();
}

struct ReleaseAssetsBundle {
    assets_dir: PathBuf,
    catalog_path: PathBuf,
    boot_tar: PathBuf,
    overlay_tar: PathBuf,
    bootstrap_tar: PathBuf,
    gpu_tar: PathBuf,
    seed_tar: PathBuf,
    expected_gpu_stage: GpuStage,
}

fn build_release_assets_bundle(fixture: &SyntheticFixture) -> ReleaseAssetsBundle {
    let root = fixture.tmp.path();
    let assets_dir = root.join("release_assets");
    fs::create_dir_all(&assets_dir).unwrap();

    let boot_tar = assets_dir.join("ios-on-switch-lmao-test-boot-24B119.tar");
    {
        let file = File::create(&boot_tar).unwrap();
        let mut tar = tar::Builder::new(file);
        append_tar_file(&mut tar, "boot.json", br#"{"build":"24B119","format":1}"#, 0o644);
        append_tar_file(
            &mut tar,
            "pretranslated/edits.json",
            br#"{"build":"24B119","files":{"/usr/libexec/demo":[{"fileoff":"0x10","stock":"deadbeef","at":0,"patched":"cafebabe"}]}}"#,
            0o644,
        );
        tar.finish().unwrap();
    }

    let helper_bytes = b"#!/bin/sh\necho switch-bootstrap-release\n";
    let installer_plist = b"synthetic-installer-plist";
    let overlay_tar = assets_dir.join("ios-on-switch-lmao-test-overlay-24B119.tar");
    {
        let file = File::create(&overlay_tar).unwrap();
        let mut tar = tar::Builder::new(file);
        append_tar_file(&mut tar, "etc/switch.conf", b"mode=release-tar\n", 0o644);
        append_tar_file(&mut tar, "usr/libexec/switch-bootstrap", helper_bytes, 0o755);
        append_tar_file(
            &mut tar,
            "Applications/SwitchInstaller.app/Info.plist",
            installer_plist,
            0o644,
        );
        tar.finish().unwrap();
    }

    let bootstrap_tar = assets_dir.join("ios-on-switch-lmao-test-bootstrap.tar");
    {
        let file = File::create(&bootstrap_tar).unwrap();
        let mut tar = tar::Builder::new(file);
        append_tar_file(&mut tar, ".procursus_strapped", b"ok\n", 0o644);
        append_tar_file(&mut tar, ".switch-bootstrap-ready", b"ok\n", 0o644);
        let mut status = String::new();
        for pkg in HELD {
            status.push_str(&format!(
                "Package: {pkg}\nStatus: install ok installed\nVersion: 1.0\n\n"
            ));
        }
        status.push_str("Package: firmware\nStatus: install ok installed\nVersion: 0.0\n\n");
        append_tar_file(&mut tar, "Library/dpkg/status", status.as_bytes(), 0o644);
        tar.finish().unwrap();
    }
    let (bs_len, bs_sha) = sha256_file(&bootstrap_tar);
    let bs_manifest = serde_json::json!({
        "schema_version": 1,
        "build": "24B119",
        "firmware": "27.2",
        "archive_sha256": bs_sha,
        "helper_sha256": hex::encode(sha256(helper_bytes)),
        "holds": HELD,
    });
    fs::write(
        fixture.boot_files_dir.join("bootstrap/manifest.json"),
        serde_json::to_vec_pretty(&bs_manifest).unwrap(),
    )
    .unwrap();

    let plugin_bytes = b"\xcf\xfa\xed\xfe-release-gpu-plugin";
    let pack_bytes = b"release-gpu-pack-payload";
    let pack_rel = "ngpu/addons/TegraGM20BMetal.pack/pipelines.bin";
    let kext_digest = "cd".repeat(32);
    let gpu_manifest = format!(
        "# gpu stage 2 tree\n\
         # kext {kext_digest}\n\
         file\t{REQUIRED_PLUGIN_PATH}\t{}\t{}\t755\t{REQUIRED_PLUGIN_PATH}\n\
         file\t{pack_rel}\t{}\t{}\t644\t{pack_rel}\n\
         pref\tmobile/Library/Preferences/com.apple.backboardd.plist\t501\tCAEnableDeepFramebuffer\tfalse\n\
         pref\troot/Library/Preferences/com.apple.backboardd.plist\t0\tCAEnableDeepFramebuffer\tfalse\n",
        hex::encode(sha256(plugin_bytes)),
        plugin_bytes.len(),
        hex::encode(sha256(pack_bytes)),
        pack_bytes.len(),
    );
    let gpu_tar = assets_dir.join("ios-on-switch-lmao-test-gpu-24B119.tar");
    {
        let file = File::create(&gpu_tar).unwrap();
        let mut tar = tar::Builder::new(file);
        append_tar_file(&mut tar, TREE_MANIFEST, gpu_manifest.as_bytes(), 0o644);
        append_tar_file(&mut tar, REQUIRED_PLUGIN_PATH, plugin_bytes, 0o755);
        append_tar_file(&mut tar, pack_rel, pack_bytes, 0o644);
        tar.finish().unwrap();
    }
    let expected_gpu_stage = GpuStage::from_tar(File::open(&gpu_tar).unwrap()).unwrap();

    let seed_tar = assets_dir.join("ios-on-switch-lmao-test-seed-24B119.tar");
    {
        let file = File::create(&seed_tar).unwrap();
        let mut tar = tar::Builder::new(file);
        append_tar_file(&mut tar, "Switch/seed/shaders.bin", b"release-seed-v2", 0o644);
        tar.finish().unwrap();
    }

    let (boot_len, boot_sha) = sha256_file(&boot_tar);
    let (ov_len, ov_sha) = sha256_file(&overlay_tar);
    let (gpu_len, gpu_sha) = sha256_file(&gpu_tar);
    let (seed_len, seed_sha) = sha256_file(&seed_tar);

    let mut catalog: serde_json::Value = serde_json::from_slice(&fs::read(&fixture.catalog_path).unwrap()).unwrap();
    catalog["builds"][0]["assets"] = serde_json::json!({
        "boot": { "name": boot_tar.file_name().unwrap().to_str().unwrap(), "size": boot_len, "sha256": boot_sha },
        "overlay": { "name": overlay_tar.file_name().unwrap().to_str().unwrap(), "size": ov_len, "sha256": ov_sha },
        "bootstrap": { "name": bootstrap_tar.file_name().unwrap().to_str().unwrap(), "size": bs_len, "sha256": bs_sha },
        "gpu": { "name": gpu_tar.file_name().unwrap().to_str().unwrap(), "size": gpu_len, "sha256": gpu_sha },
        "seed": { "name": seed_tar.file_name().unwrap().to_str().unwrap(), "size": seed_len, "sha256": seed_sha },
    });
    let catalog_path = root.join("release-catalog.json");
    let cat_bytes = serde_json::to_vec_pretty(&catalog).unwrap();
    Catalog::parse(&cat_bytes).unwrap();
    fs::write(&catalog_path, cat_bytes).unwrap();

    ReleaseAssetsBundle {
        assets_dir,
        catalog_path,
        boot_tar,
        overlay_tar,
        bootstrap_tar,
        gpu_tar,
        seed_tar,
        expected_gpu_stage,
    }
}

fn create_install_card(path: &Path) -> u64 {
    let fat_sectors: u64 = 160_000;
    let target_first = FAT_FIRST_LBA + fat_sectors;
    let target_sectors: u64 = 4_194_304;
    let total_sectors = target_first + target_sectors + 4096;
    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    let target_e = make_gpt_entry(LINUX_TYPE, 2, SETUP_NAME, target_first, target_sectors);
    create_gpt_card_file(path, total_sectors, &[(0, fat_e), (1, target_e)]);
    format_fat32_region(path, FAT_FIRST_LBA, fat_sectors, 1);
    target_first
}

fn read_apfs_file(src: &FileSource, fs_vol: &Fs<'_>, path: &str) -> (u32, u32, u16, Vec<u8>) {
    let oid = fs_vol
        .lookup(path)
        .unwrap()
        .unwrap_or_else(|| panic!("{path} missing in volume"));
    let (entry, _) = fs_vol.entry(oid, path.to_owned(), None).unwrap();
    let mut bytes = Vec::new();
    if let Some(ds) = &entry.data {
        stream::read(src, ds, &mut |chunk| bytes.extend_from_slice(chunk)).unwrap();
    }
    (
        entry.inode.val.owner.get(),
        entry.inode.val.group.get(),
        entry.inode.val.mode.get() & 0o7777,
        bytes,
    )
}

#[test]
fn e2e_install_from_catalog_and_assets_dir_without_overrides() {
    let fixture = build_synthetic_fixture(false);
    let bundle = build_release_assets_bundle(&fixture);
    let img = fixture.tmp.path().join("auto-assets.img");
    let target_first = create_install_card(&img);

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    req.build_id = Some("24B119".to_owned());
    req.work_dir = Some(fixture.work_dir.clone());
    req.log_path = Some(fixture.log_path.clone());
    req.local.catalog_path = Some(bundle.catalog_path.clone());
    req.local.assets_dir = Some(bundle.assets_dir.clone());
    req.local.os_dmg = Some(fixture.os_path.clone());
    req.local.system_cryptex = Some(fixture.sys_cryptex_path.clone());
    req.local.app_cryptex = Some(fixture.app_cryptex_path.clone());
    req.local.boot_files_dir = Some(fixture.boot_files_dir.clone());

    let mut sink = NoopSink;
    let summary = run(&req, None, &mut sink, CancelToken::new()).unwrap();
    assert_eq!(summary.build.as_deref(), Some("24B119"));
    assert_eq!(summary.boot_files_written, 3);

    let src = FileSource::open(&img, target_first * SECTOR as u64).unwrap();
    let container = Container::open(&src).unwrap();
    let audit = switch_apfs_read::audit(&container, &src).unwrap();
    assert!(audit.ok(), "APFS audit failed:\n{audit}");

    let data_vol = container.volume(parse_uuid(DATA_VOLUME_UUID_STR).unwrap()).unwrap();
    let data_fs = Fs::open(&src, data_vol).unwrap();

    let (_, _, helper_mode, _) = read_apfs_file(&src, &data_fs, "/Switch/overlay/usr/libexec/switch-bootstrap");
    assert_eq!(helper_mode, 0o755);
    let (_, _, conf_mode, conf_bytes) = read_apfs_file(&src, &data_fs, "/Switch/overlay/etc/switch.conf");
    assert_eq!(conf_mode, 0o644);
    assert_eq!(conf_bytes, b"mode=release-tar\n");
    assert!(
        data_fs
            .lookup("/Switch/overlay/Applications/SwitchInstaller.app/Info.plist")
            .unwrap()
            .is_some()
    );
    assert!(
        data_fs
            .lookup("/Switch/bootstrap/24B119/manifest.json")
            .unwrap()
            .is_some()
    );
    assert!(data_fs.lookup("/jb").unwrap().is_some());

    let (uid, gid, mode, got_plugin) = read_apfs_file(&src, &data_fs, &format!("/{REQUIRED_PLUGIN_PATH}"));
    assert_eq!((uid, gid, mode), (0, 0, 0o755));
    assert_eq!(got_plugin, b"\xcf\xfa\xed\xfe-release-gpu-plugin");

    let (_, _, _, got_gz) = read_apfs_file(&src, &data_fs, &format!("/{STATE_PATH}"));
    let mut tsv = String::new();
    GzDecoder::new(got_gz.as_slice()).read_to_string(&mut tsv).unwrap();
    assert_eq!(tsv, bundle.expected_gpu_stage.stage_tsv());

    let (_, _, _, got_icon) = read_apfs_file(&src, &data_fs, &format!("/{ICON_CACHE_RECORD_PATH}"));
    assert_eq!(
        got_icon,
        format!("{}\n", bundle.expected_gpu_stage.icon_build()).as_bytes()
    );

    let (uid, gid, _, raw_plist) =
        read_apfs_file(&src, &data_fs, "/mobile/Library/Preferences/com.apple.backboardd.plist");
    assert_eq!((uid, gid), (501, 501));
    let dict = PlistValue::from_reader(Cursor::new(&raw_plist))
        .unwrap()
        .into_dictionary()
        .unwrap();
    assert_eq!(
        dict.get("CAEnableDeepFramebuffer").and_then(PlistValue::as_boolean),
        Some(false)
    );

    let (_, _, _, seed_bytes) = read_apfs_file(&src, &data_fs, "/Switch/seed/shaders.bin");
    assert_eq!(seed_bytes, b"release-seed-v2");
}

#[test]
fn corrupted_release_assets_in_assets_dir_are_refused() {
    let fixture = build_synthetic_fixture(false);
    let bundle = build_release_assets_bundle(&fixture);
    let img = fixture.tmp.path().join("corrupt-assets.img");
    create_install_card(&img);

    let orig_overlay = fs::read(&bundle.overlay_tar).unwrap();
    let mut bad_overlay = orig_overlay.clone();
    let last = bad_overlay.len() - 1;
    bad_overlay[last] ^= 0x01;
    fs::write(&bundle.overlay_tar, &bad_overlay).unwrap();

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    req.build_id = Some("24B119".to_owned());
    req.work_dir = Some(fixture.work_dir.clone());
    req.log_path = Some(fixture.log_path.clone());
    req.local.catalog_path = Some(bundle.catalog_path.clone());
    req.local.assets_dir = Some(bundle.assets_dir.clone());
    req.local.os_dmg = Some(fixture.os_path.clone());
    req.local.system_cryptex = Some(fixture.sys_cryptex_path.clone());
    req.local.app_cryptex = Some(fixture.app_cryptex_path.clone());
    req.local.boot_files_dir = Some(fixture.boot_files_dir.clone());

    let mut sink = NoopSink;
    let err = run(&req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(err, EngineError::Catalog(switch_catalog::Error::AssetMismatch { .. })),
        "expected AssetMismatch on corrupted overlay tar, got {err:?}"
    );
    fs::write(&bundle.overlay_tar, orig_overlay).unwrap();

    let orig_gpu = fs::read(&bundle.gpu_tar).unwrap();
    let mut bad_gpu = orig_gpu.clone();
    let last = bad_gpu.len() - 1;
    bad_gpu[last] ^= 0x01;
    fs::write(&bundle.gpu_tar, &bad_gpu).unwrap();

    let err = run(&req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(err, EngineError::Catalog(switch_catalog::Error::AssetMismatch { .. })),
        "expected AssetMismatch on corrupted gpu tar, got {err:?}"
    );
    fs::write(&bundle.gpu_tar, orig_gpu).unwrap();

    let orig_bs = fs::read(&bundle.bootstrap_tar).unwrap();
    let mut bad_bs = orig_bs.clone();
    let last = bad_bs.len() - 1;
    bad_bs[last] ^= 0x01;
    fs::write(&bundle.bootstrap_tar, &bad_bs).unwrap();

    let err = run(&req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(
            err,
            EngineError::Bootstrap(switch_image_bootstrap::Error::Digest(_))
                | EngineError::Catalog(switch_catalog::Error::AssetMismatch { .. })
        ),
        "expected digest refusal on corrupted bootstrap tar, got {err:?}"
    );
    fs::write(&bundle.bootstrap_tar, orig_bs).unwrap();
}

#[test]
fn resolve_release_asset_cache_edits_and_tar_overrides() {
    let fixture = build_synthetic_fixture(false);
    let bundle = build_release_assets_bundle(&fixture);
    let catalog = Catalog::parse(&fs::read(&bundle.catalog_path).unwrap()).unwrap();
    let build = catalog.record("24B119").unwrap().plan().unwrap();

    let mut sink = NoopSink;
    let mut progress = EngineProgress::new(&mut sink, None, CancelToken::new());
    let resolved_boot = resolve_release_asset(
        &catalog,
        &build,
        "boot",
        None,
        Some(&bundle.assets_dir),
        &fixture.work_dir,
        &mut progress,
    )
    .unwrap()
    .unwrap();
    assert_eq!(resolved_boot, bundle.boot_tar);

    let edits = read_pretranslated_edits_from_boot_tar(&resolved_boot)
        .unwrap()
        .expect("boot tar carries pretranslated/edits.json");
    assert!(std::str::from_utf8(&edits).unwrap().contains("/usr/libexec/demo"));

    let cache_assets = fixture.work_dir.join("cache").join(&catalog.release).join("assets");
    fs::create_dir_all(&cache_assets).unwrap();
    let cached_overlay = cache_assets.join(bundle.overlay_tar.file_name().unwrap());
    fs::copy(&bundle.overlay_tar, &cached_overlay).unwrap();
    let reused = resolve_release_asset(
        &catalog,
        &build,
        "overlay",
        None,
        None,
        &fixture.work_dir,
        &mut progress,
    )
    .unwrap()
    .unwrap();
    assert_eq!(reused, cached_overlay);

    let absent_cat = Catalog::parse(&fs::read(&fixture.catalog_path).unwrap()).unwrap();
    let absent_build = absent_cat.record("24B119").unwrap().plan().unwrap();
    assert_eq!(
        resolve_release_asset(
            &absent_cat,
            &absent_build,
            "overlay",
            None,
            None,
            &fixture.work_dir,
            &mut progress,
        )
        .unwrap(),
        None
    );

    let img = fixture.tmp.path().join("tar-overrides.img");
    let target_first = create_install_card(&img);
    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    req.build_id = Some("24B119".to_owned());
    req.work_dir = Some(fixture.work_dir.clone());
    req.log_path = Some(fixture.log_path.clone());
    req.local.catalog_path = Some(fixture.catalog_path.clone());
    req.local.os_dmg = Some(fixture.os_path.clone());
    req.local.system_cryptex = Some(fixture.sys_cryptex_path.clone());
    req.local.app_cryptex = Some(fixture.app_cryptex_path.clone());
    req.local.boot_files_dir = Some(fixture.boot_files_dir.clone());
    req.local.overlay_dir = Some(bundle.overlay_tar);
    req.local.bootstrap_dir = Some(bundle.bootstrap_tar);
    req.local.gpu_dir = Some(bundle.gpu_tar);
    req.local.shader_seed_dir = Some(bundle.seed_tar);

    let mut run_sink = NoopSink;
    run(&req, None, &mut run_sink, CancelToken::new()).unwrap();
    let src = FileSource::open(&img, target_first * SECTOR as u64).unwrap();
    let container = Container::open(&src).unwrap();
    assert!(switch_apfs_read::audit(&container, &src).unwrap().ok());
}
