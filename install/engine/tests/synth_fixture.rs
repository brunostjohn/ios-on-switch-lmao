// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, File};
use std::path::{Path, PathBuf};

use switch_apfs_names::UnicodeVersion;
use switch_catalog::Catalog;
use switch_engine::RunRequest;
use switch_image::parse_uuid;
use switch_image_bootstrap::HELD;
use tempfile::TempDir;

use crate::synth_apfs::{sha256, sha256_file, write_synthetic_os_container};
use crate::synth_cryptex::write_synthetic_sealed_cryptex;

pub const SYSTEM_UUID_STR: &str = "11111111-2222-4333-8444-555555555555";
pub const SYS_CRYPTEX_UUID_STR: &str = "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE";
pub const APP_CRYPTEX_UUID_STR: &str = "22222222-3333-4444-8555-666666666666";
pub const SYS_CRYPTEX_STAGE: &str = "sys-test.dmg";
pub const APP_CRYPTEX_STAGE: &str = "app-test.dmg";

pub struct SyntheticFixture {
    pub tmp: TempDir,
    pub catalog_path: PathBuf,
    pub os_path: PathBuf,
    pub sys_cryptex_path: PathBuf,
    pub app_cryptex_path: PathBuf,
    pub boot_files_dir: PathBuf,
    pub overlay_dir: PathBuf,
    pub bootstrap_dir: PathBuf,
    pub gpu_dir: PathBuf,
    pub seed_dir: PathBuf,
    pub pt_os_dir: PathBuf,
    pub pt_sys_dir: PathBuf,
    pub pt_app_dir: PathBuf,
    pub work_dir: PathBuf,
    pub log_path: PathBuf,
}

pub fn build_synthetic_fixture(pretranslate: bool) -> SyntheticFixture {
    let tmp = TempDir::new().unwrap();
    let root = tmp.path().to_path_buf();

    let os_path = root.join("os.apfs");
    let sys_cryptex_path = root.join("sys_cryptex.apfs");
    let app_cryptex_path = root.join("app_cryptex.apfs");

    let sys_uuid = parse_uuid(SYSTEM_UUID_STR).unwrap();
    let sys_cx_uuid = parse_uuid(SYS_CRYPTEX_UUID_STR).unwrap();
    let app_cx_uuid = parse_uuid(APP_CRYPTEX_UUID_STR).unwrap();

    write_synthetic_os_container(&os_path, sys_uuid, UnicodeVersion::IOS_27);
    write_synthetic_sealed_cryptex(
        &sys_cryptex_path,
        sys_cx_uuid,
        "SystemOS",
        &[("sys_bin", b"original-sys-cryptex-payload")],
    );
    write_synthetic_sealed_cryptex(
        &app_cryptex_path,
        app_cx_uuid,
        "AppOS",
        &[("app_bin", b"original-app-cryptex-payload")],
    );

    let boot_files_dir = root.join("boot_files");
    fs::create_dir_all(boot_files_dir.join("switchroot/ios")).unwrap();
    fs::create_dir_all(boot_files_dir.join("bootloader/ini")).unwrap();
    fs::create_dir_all(boot_files_dir.join("bootloader/res")).unwrap();
    fs::create_dir_all(boot_files_dir.join("overlay/usr/libexec")).unwrap();
    fs::create_dir_all(boot_files_dir.join("bootstrap")).unwrap();
    fs::write(boot_files_dir.join("switchroot/ios/bl33.bin"), b"BL33-PAYLOAD").unwrap();
    fs::write(boot_files_dir.join("bootloader/ini/ios.ini"), b"[iOS]\npayload=1\n").unwrap();
    fs::write(boot_files_dir.join("bootloader/res/icon_ios_hue.bmp"), b"BMICON").unwrap();
    fs::write(boot_files_dir.join("overlay/usr/libexec/installd"), b"patched-installd").unwrap();
    fs::write(boot_files_dir.join("bootstrap/manifest.json"), b"{}\n").unwrap();
    fs::write(boot_files_dir.join("bootstrap/payload.tar"), b"bootstrap-tar").unwrap();

    let installer_plist = b"synthetic-installer-plist";
    let installer_pin = serde_json::json!({
        "schema": 1,
        "identifier": "com.brunostjohn.switch.installer",
        "files": {
            "Info.plist": hex::encode(sha256(installer_plist)),
        },
    });
    fs::write(
        boot_files_dir.join("installer-bundle.json"),
        serde_json::to_vec_pretty(&installer_pin).unwrap(),
    )
    .unwrap();

    let overlay_dir = root.join("overlay");
    fs::create_dir_all(overlay_dir.join("etc")).unwrap();
    fs::write(overlay_dir.join("etc/switch.conf"), b"mode=test\n").unwrap();
    fs::create_dir_all(overlay_dir.join("Applications/SwitchInstaller.app")).unwrap();
    fs::write(
        overlay_dir.join("Applications/SwitchInstaller.app/Info.plist"),
        installer_plist,
    )
    .unwrap();

    let bootstrap_dir = root.join("bootstrap_pkg");
    write_synthetic_bootstrap_dir(&bootstrap_dir, &overlay_dir, "24B119", "27.2");

    let gpu_dir = root.join("gpu");
    fs::create_dir_all(gpu_dir.join("Switch/gpu")).unwrap();
    fs::write(gpu_dir.join("Switch/gpu/driver.meta"), b"gpu-bundle-v1").unwrap();

    let seed_dir = root.join("seed");
    fs::create_dir_all(seed_dir.join("Switch/seed")).unwrap();
    fs::write(seed_dir.join("Switch/seed/shaders.bin"), b"shader-seed-v1").unwrap();

    let pt_os_dir = root.join("pt_os");
    write_os_delta(&pt_os_dir);

    let pt_sys_dir = root.join("pt_sys");
    write_cryptex_delta(
        &pt_sys_dir,
        "system_cryptex",
        "/sys_bin",
        "files/sys_bin",
        b"original-sys-cryptex-payload",
        b"translated-sys-cryptex-payload-v2",
    );

    let pt_app_dir = root.join("pt_app");
    write_cryptex_delta(
        &pt_app_dir,
        "app_cryptex",
        "/app_bin",
        "files/app_bin",
        b"original-app-cryptex-payload",
        b"translated-app-cryptex-payload-v2",
    );

    let (os_len, os_sha) = sha256_file(&os_path);
    let (sys_len, sys_sha) = sha256_file(&sys_cryptex_path);
    let (app_len, app_sha) = sha256_file(&app_cryptex_path);
    let catalog_path = root.join("catalog.json");
    write_synthetic_catalog(
        &catalog_path,
        pretranslate,
        (os_len, &os_sha),
        (sys_len, &sys_sha),
        (app_len, &app_sha),
    );

    let work_dir = root.join("work");
    let log_path = root.join("logs/run.log");
    SyntheticFixture {
        tmp,
        catalog_path,
        os_path,
        sys_cryptex_path,
        app_cryptex_path,
        boot_files_dir,
        overlay_dir,
        bootstrap_dir,
        gpu_dir,
        seed_dir,
        pt_os_dir,
        pt_sys_dir,
        pt_app_dir,
        work_dir,
        log_path,
    }
}

impl SyntheticFixture {
    pub fn apply_to_request(&self, req: &mut RunRequest, pretranslate: bool) {
        req.build_id = Some("24B119".to_owned());
        req.work_dir = Some(self.work_dir.clone());
        req.log_path = Some(self.log_path.clone());
        req.local.catalog_path = Some(self.catalog_path.clone());
        req.local.os_dmg = Some(self.os_path.clone());
        req.local.system_cryptex = Some(self.sys_cryptex_path.clone());
        req.local.app_cryptex = Some(self.app_cryptex_path.clone());
        req.local.boot_files_dir = Some(self.boot_files_dir.clone());
        req.local.overlay_dir = Some(self.overlay_dir.clone());
        req.local.bootstrap_dir = Some(self.bootstrap_dir.clone());
        req.local.gpu_dir = Some(self.gpu_dir.clone());
        req.local.shader_seed_dir = Some(self.seed_dir.clone());
        if pretranslate {
            req.local.pretranslated_os_dir = Some(self.pt_os_dir.clone());
            req.local.pretranslated_system_cryptex_dir = Some(self.pt_sys_dir.clone());
            req.local.pretranslated_app_cryptex_dir = Some(self.pt_app_dir.clone());
        }
    }
}

fn write_os_delta(dir: &Path) {
    fs::create_dir_all(dir.join("files")).unwrap();
    let var_new = b"var-translated-binary";
    let sys_new = b"system-translated-binary";
    fs::write(dir.join("files/var_tool"), var_new).unwrap();
    fs::write(dir.join("files/demo_bin"), sys_new).unwrap();
    let manifest = serde_json::json!({
        "role": "os",
        "translator": "test",
        "files": [
            {
                "path": "/private/var/db/tool",
                "file": "files/var_tool",
                "sha256_in": hex::encode(sha256(b"var-original-binary")),
                "sha256_out": hex::encode(sha256(var_new)),
                "cdhash": ["11".repeat(20)]
            },
            {
                "path": "/usr/libexec/demo_bin",
                "file": "files/demo_bin",
                "sha256_in": hex::encode(sha256(b"system-original-binary")),
                "sha256_out": hex::encode(sha256(sys_new)),
                "cdhash": ["22".repeat(20)]
            }
        ]
    });
    fs::write(
        dir.join("os.manifest.json"),
        serde_json::to_vec_pretty(&manifest).unwrap(),
    )
    .unwrap();
    fs::write(
        dir.join("os.cdhash-pairs"),
        format!("{} {}\n", "aa".repeat(20), "22".repeat(20)),
    )
    .unwrap();
}

fn write_cryptex_delta(dir: &Path, role: &str, vol_path: &str, rel_file: &str, orig: &[u8], new_bytes: &[u8]) {
    fs::create_dir_all(dir.join("files")).unwrap();
    fs::write(dir.join(rel_file), new_bytes).unwrap();
    let manifest = serde_json::json!({
        "files": [{
            "path": vol_path,
            "file": rel_file,
            "sha256_in": hex::encode(sha256(orig)),
            "sha256_out": hex::encode(sha256(new_bytes))
        }]
    });
    fs::write(
        dir.join(format!("{role}.manifest.json")),
        serde_json::to_vec_pretty(&manifest).unwrap(),
    )
    .unwrap();
}

fn write_synthetic_bootstrap_dir(package_dir: &Path, overlay_dir: &Path, build: &str, version: &str) {
    let helper_bytes = b"#!/bin/sh\necho switch-bootstrap\n";
    let helper_dir = overlay_dir.join("usr/libexec");
    fs::create_dir_all(&helper_dir).unwrap();
    fs::write(helper_dir.join("switch-bootstrap"), helper_bytes).unwrap();
    let helper_sha = hex::encode(sha256(helper_bytes));

    let pkg_helper_dir = package_dir.join("overlay/usr/libexec");
    fs::create_dir_all(&pkg_helper_dir).unwrap();
    fs::write(pkg_helper_dir.join("switch-bootstrap"), helper_bytes).unwrap();

    let boot_dir = package_dir.join("bootstrap");
    fs::create_dir_all(&boot_dir).unwrap();
    let tar_path = boot_dir.join("payload.tar");
    {
        let file = File::create(&tar_path).unwrap();
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
    let tar_sha = sha256_file(&tar_path).1;
    let manifest = serde_json::json!({
        "schema_version": 1,
        "build": build,
        "firmware": version,
        "archive_sha256": tar_sha,
        "helper_sha256": helper_sha,
        "holds": HELD,
    });
    fs::write(
        boot_dir.join("manifest.json"),
        serde_json::to_vec_pretty(&manifest).unwrap(),
    )
    .unwrap();
}

fn append_tar_file<W: std::io::Write>(tar: &mut tar::Builder<W>, path: &str, data: &[u8], mode: u32) {
    let mut header = tar::Header::new_gnu();
    header.set_size(data.len() as u64);
    header.set_mode(mode);
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_cksum();
    tar.append_data(&mut header, path, data).unwrap();
}

fn write_synthetic_catalog(path: &Path, pretranslate: bool, os: (u64, &str), sys_cx: (u64, &str), app_cx: (u64, &str)) {
    let dummy32 = "00".repeat(32);
    let dummy48 = "00".repeat(48);
    let disk_member = |name: &str, (size, sha): (u64, &str)| {
        serde_json::json!({
            "member": name, "component": name, "offset": 0, "stored_size": size, "size": size,
            "sha256": sha, "content_sha256": sha, "deflated": false, "manifest_digest": dummy48,
            "aea": null, "container": { "format": "raw", "size": size, "sha256": sha }, "im4p": null
        })
    };
    let im4p_member = |name: &str, fourcc: &str| {
        serde_json::json!({
            "member": name, "component": name, "offset": 0, "stored_size": 16, "size": 16,
            "sha256": dummy32, "content_sha256": dummy32, "deflated": false, "manifest_digest": dummy48,
            "aea": null, "container": null, "im4p": { "fourcc": fourcc, "payload_size": 8, "payload_sha256": dummy32 }
        })
    };
    let mut steps = serde_json::Map::new();
    steps.insert("prelink".into(), "fileset".into());
    steps.insert("boot_manifest_hash".into(), "sha384".into());
    let mut pretranslated = serde_json::Map::new();
    if pretranslate {
        steps.insert("pretranslate".into(), "a64x".into());
        pretranslated.insert("os.trustcache".into(), serde_json::Value::String(dummy32.clone()));
    }
    let catalog = serde_json::json!({
        "format": 1,
        "release": "v0.0.0-test",
        "builds": [{
            "build": "24B119",
            "version": "27.2",
            "ios": 27,
            "xnu": "12377.121.6",
            "product": "iPad12,1",
            "device_class": "j120ap",
            "ipsw": { "url": "https://example.invalid/test.ipsw", "size": 1024 },
            "steps": steps,
            "members": {
                "os": disk_member("os.dmg", os),
                "system_cryptex": disk_member("sys.dmg", sys_cx),
                "app_cryptex": disk_member("app.dmg", app_cx),
                "kernelcache": im4p_member("kernelcache", "krnl"),
                "device_tree": im4p_member("dtre", "dtre"),
                "static_trust_cache": im4p_member("stc", "trst"),
                "system_cryptex_trust_cache": im4p_member("sysc", "rtsc"),
                "app_cryptex_trust_cache": im4p_member("appc", "trst"),
            },
            "volumes": {
                "os_volume_uuid": SYSTEM_UUID_STR,
                "system_cryptex_uuid": SYS_CRYPTEX_UUID_STR,
                "system_cryptex_image": SYS_CRYPTEX_STAGE,
                "app_cryptex_uuid": APP_CRYPTEX_UUID_STR,
                "app_cryptex_image": APP_CRYPTEX_STAGE,
            },
            "derived": {
                "kernel_virt_base": "0xfffffe0007004000",
                "quartzcore_metallib_sha256": dummy32,
                "launchd_catalog_sha256": null,
                "launchd_catalog_signature_sha256": null,
                "pretranslated": pretranslated,
            },
            "assets": {
                "boot": { "name": "boot-24B119.tar", "size": 512, "sha256": dummy32 }
            }
        }]
    });
    let bytes = serde_json::to_vec_pretty(&catalog).unwrap();
    Catalog::parse(&bytes).expect("synthetic catalog must parse cleanly");
    fs::write(path, bytes).unwrap();
}
