// SPDX-License-Identifier: GPL-3.0-or-later
pub mod asset;
pub mod error;
pub mod ffi;
mod fsutil;
pub mod pipeline;
pub mod smoke;
mod stages;

pub use asset::{
    EXPECTED_SPLIT_BUCKETS, MANIFEST_FORMAT, SeedAppLibrary, SeedAssetFile, SeedAssetManifest, SeedBucketCounts,
    UnpackedSeedAsset, is_seed_asset_tar, parse_stack_key_hex, read_bundle_stack_key, read_seed_asset_manifest,
    unpack_seed_asset,
};
pub use error::{Result, SeedError};
pub use pipeline::{QC_REL_PATH, SeedInputs, SeedOptions, SeedOutput, StagedSeedFile, build_seed};
pub use smoke::{SeedSmokeEntry, SeedSmokeReport, run_seed_repro_smoke};
pub use switch_seed_plan::Depth;

#[cfg(test)]
mod tests {
    use std::fs::File;
    use std::path::Path;

    use sha2::{Digest, Sha256};

    use super::*;

    fn append_tar(tar: &mut tar::Builder<File>, name: &str, data: &[u8]) {
        let mut h = tar::Header::new_ustar();
        h.set_size(data.len() as u64);
        h.set_mode(0o644);
        h.set_uid(0);
        h.set_gid(0);
        h.set_mtime(0);
        h.set_cksum();
        tar.append_data(&mut h, name, data).unwrap();
    }

    fn write_test_seed_tar(path: &Path, build: &str, stack: &str, qc_sha: &str) {
        let mut files = Vec::new();
        let mut entries: Vec<(String, Vec<u8>)> = vec![
            ("raw/own.metallib".into(), b"MTLB-dummy".to_vec()),
            ("raw/context.log".into(), b"init\n".to_vec()),
            ("raw/draw_context.txt".into(), b"".to_vec()),
            ("raw/corpus.log".into(), b"".to_vec()),
            ("split/corpus-report.txt".into(), b"".to_vec()),
        ];
        for i in 0..EXPECTED_SPLIT_BUCKETS {
            entries.push((format!("split/bucket{i}.log"), Vec::new()));
        }
        for (p, data) in &entries {
            files.push(SeedAssetFile {
                path: p.clone(),
                sha256: hex::encode(Sha256::digest(data)),
                size: data.len() as u64,
            });
        }
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
        append_tar(&mut tar, "manifest.json", &m_bytes);
        for (p, data) in &entries {
            append_tar(&mut tar, p, data);
        }
        tar.finish().unwrap();
    }

    #[test]
    fn seed_asset_tar_unpacks_and_checks_stack_and_qc() {
        let tmp = tempfile::tempdir().unwrap();
        let tar_path = tmp.path().join("seed.tar");
        let qc = "aa".repeat(32);
        let stack = "0123456789abcdef";
        write_test_seed_tar(&tar_path, "24B119", stack, &qc);

        assert!(is_seed_asset_tar(&tar_path).unwrap());

        let unpacked = unpack_seed_asset(&tar_path, "24B119", &qc, Some(stack), &tmp.path().join("u1")).unwrap();
        assert_eq!(unpacked.split_buckets.len(), EXPECTED_SPLIT_BUCKETS);
        assert_eq!(unpacked.manifest.stack_key, stack);

        let err = unpack_seed_asset(
            &tar_path,
            "24B119",
            &qc,
            Some("fedcba9876543210"),
            &tmp.path().join("u2"),
        )
        .unwrap_err();
        assert!(matches!(err, SeedError::StackKeyMismatch { .. }));

        let err = unpack_seed_asset(&tar_path, "21H461", &qc, Some(stack), &tmp.path().join("u3")).unwrap_err();
        assert!(matches!(err, SeedError::BuildMismatch { .. }));
    }

    #[test]
    fn read_bundle_stack_key_parses_plist() {
        let xml = br#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>SwitchShaderStack</key><string>4e5d877fb70856dd</string></dict></plist>"#;
        assert_eq!(read_bundle_stack_key(xml).unwrap().as_deref(), Some("4e5d877fb70856dd"));
    }

    #[test]
    fn runtime_init_links_mesa_and_translate() {
        let _guard = ffi::init_runtime(16 * 1024 * 1024);
        assert!(ffi::alloc_scope() > 0);
    }

    #[test]
    fn rejects_windows_unsafe_paths_undeclared_entries_and_embedded_escapes() {
        for bad in [
            "C:foo",
            "C:/foo",
            "foo:stream",
            "raw/CON",
            "raw/NUL.txt",
            "raw/COM1",
            "raw/LPT9.log",
            "raw/trailing.",
            "raw/trailing ",
            "raw\\back",
            "raw/../escape",
            "/leading",
        ] {
            assert!(asset::check_rel_path(bad).is_err(), "expected {bad:?} to be rejected");
        }

        let tmp = tempfile::tempdir().unwrap();
        let qc = "aa".repeat(32);
        let stack = "0123456789abcdef";

        // Undeclared extra tar entry must be refused.
        let extra_tar = tmp.path().join("extra.tar");
        write_test_seed_tar(&extra_tar, "24B119", stack, &qc);
        let mut orig_entries = Vec::new();
        {
            let mut ar = tar::Archive::new(File::open(&extra_tar).unwrap());
            for e in ar.entries().unwrap() {
                let mut e = e.unwrap();
                let name = e.path().unwrap().to_str().unwrap().to_owned();
                let mut data = Vec::new();
                std::io::Read::read_to_end(&mut e, &mut data).unwrap();
                orig_entries.push((name, data));
            }
        }
        {
            let file = File::create(&extra_tar).unwrap();
            let mut tar = tar::Builder::new(file);
            for (name, data) in &orig_entries {
                append_tar(&mut tar, name, data);
            }
            append_tar(&mut tar, "raw/undeclared.bin", b"sneaky");
            tar.finish().unwrap();
        }
        let err = unpack_seed_asset(&extra_tar, "24B119", &qc, Some(stack), &tmp.path().join("u_extra")).unwrap_err();
        assert!(matches!(err, SeedError::Asset(_)));

        // Mismatched manifest.kind must be refused.
        let bad_kind_tar = tmp.path().join("bad_kind.tar");
        {
            let file = File::create(&bad_kind_tar).unwrap();
            let mut tar = tar::Builder::new(file);
            for (name, data) in &orig_entries {
                if name == "manifest.json" {
                    let mut m: SeedAssetManifest = serde_json::from_slice(data).unwrap();
                    m.kind = "21H461".to_owned();
                    append_tar(&mut tar, name, &serde_json::to_vec(&m).unwrap());
                } else {
                    append_tar(&mut tar, name, data);
                }
            }
            tar.finish().unwrap();
        }
        let err = unpack_seed_asset(&bad_kind_tar, "24B119", &qc, Some(stack), &tmp.path().join("u_kind")).unwrap_err();
        assert!(matches!(err, SeedError::Asset(_)));

        // Unreferenced or escaping manifest.embedded entry must be refused.
        let bad_emb_tar = tmp.path().join("bad_emb.tar");
        {
            let file = File::create(&bad_emb_tar).unwrap();
            let mut tar = tar::Builder::new(file);
            for (name, data) in &orig_entries {
                if name == "manifest.json" {
                    let mut m: SeedAssetManifest = serde_json::from_slice(data).unwrap();
                    m.embedded.push(SeedAssetFile {
                        path: "../outside.metallib".to_owned(),
                        sha256: "bb".repeat(32),
                        size: 4,
                    });
                    append_tar(&mut tar, name, &serde_json::to_vec(&m).unwrap());
                } else {
                    append_tar(&mut tar, name, data);
                }
            }
            tar.finish().unwrap();
        }
        let err = unpack_seed_asset(&bad_emb_tar, "24B119", &qc, Some(stack), &tmp.path().join("u_emb")).unwrap_err();
        assert!(matches!(err, SeedError::Asset(_)));

        // Zero native_buckets must fail cleanly before work starts.
        let dummy = tmp.path().join("dummy");
        std::fs::write(&dummy, b"").unwrap();
        let inputs = SeedInputs {
            build: "24B119",
            qc_sha256: &qc,
            seed_tar: &dummy,
            seed_tar_sha256: &qc,
            os_container: &dummy,
            system_cryptex: &dummy,
            app_cryptex: &dummy,
            os_sha256: None,
            system_cryptex_sha256: None,
            app_cryptex_sha256: None,
            bundle_stack_key: Some(stack),
            work_dir: tmp.path(),
        };
        let opts = SeedOptions {
            native_buckets: 0,
            ..SeedOptions::default()
        };
        let cancel = std::sync::atomic::AtomicBool::new(false);
        assert!(build_seed(&inputs, &opts, &cancel, |_, _, _, _| {}).is_err());
    }

    #[test]
    fn app_libraries_unpack_dedup_and_reject_invalid() {
        let tmp = tempfile::tempdir().unwrap();
        let qc = "aa".repeat(32);
        let stack = "0123456789abcdef";
        let app_bytes = b"MTLB-app-dummy";
        let app_sha = hex::encode(Sha256::digest(app_bytes));
        let app_rel = format!("raw/apps/{app_sha}.metallib");

        let make_tar = |name: &str, app_libs: Vec<SeedAppLibrary>, include_blob: bool| {
            let mut entries: Vec<(String, Vec<u8>)> = vec![
                ("raw/own.metallib".into(), b"MTLB-dummy".to_vec()),
                ("raw/context.log".into(), b"init\n".to_vec()),
                ("raw/draw_context.txt".into(), b"".to_vec()),
                ("raw/corpus.log".into(), b"".to_vec()),
                ("split/corpus-report.txt".into(), b"".to_vec()),
            ];
            for i in 0..EXPECTED_SPLIT_BUCKETS {
                entries.push((format!("split/bucket{i}.log"), Vec::new()));
            }
            if include_blob {
                entries.push((app_rel.clone(), app_bytes.to_vec()));
            }
            let files = entries
                .iter()
                .map(|(p, data)| SeedAssetFile {
                    path: p.clone(),
                    sha256: hex::encode(Sha256::digest(data)),
                    size: data.len() as u64,
                })
                .collect();
            let manifest = SeedAssetManifest {
                format: MANIFEST_FORMAT,
                build: "24B119".to_owned(),
                kind: "24B119".to_owned(),
                stack_key: stack.to_owned(),
                quartzcore_metallib_sha256: qc.clone(),
                bucket_counts: SeedBucketCounts {
                    lines: vec![0; EXPECTED_SPLIT_BUCKETS],
                    split: EXPECTED_SPLIT_BUCKETS,
                },
                embedded: Vec::new(),
                app_libraries: app_libs,
                files,
            };
            let tar_path = tmp.path().join(name);
            let file = File::create(&tar_path).unwrap();
            let mut tar = tar::Builder::new(file);
            append_tar(&mut tar, "manifest.json", &serde_json::to_vec(&manifest).unwrap());
            for (p, data) in &entries {
                append_tar(&mut tar, p, data);
            }
            tar.finish().unwrap();
            tar_path
        };

        let ok_tar = make_tar(
            "ok_apps.tar",
            vec![
                SeedAppLibrary {
                    guest_path: "Applications/SwitchHWTest.app/default.metallib".to_owned(),
                    path: app_rel.clone(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                },
                SeedAppLibrary {
                    guest_path: "Applications/SwitchHWTest27e.app/default.metallib".to_owned(),
                    path: app_rel.clone(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                },
            ],
            true,
        );
        let unpacked = unpack_seed_asset(&ok_tar, "24B119", &qc, Some(stack), &tmp.path().join("u_ok")).unwrap();
        assert_eq!(unpacked.app_libraries.len(), 2);
        assert_eq!(
            unpacked.app_libraries[0].0,
            "Applications/SwitchHWTest.app/default.metallib"
        );
        assert_eq!(
            unpacked.app_libraries[1].0,
            "Applications/SwitchHWTest27e.app/default.metallib"
        );
        assert_eq!(unpacked.app_libraries[0].1, unpacked.app_libraries[1].1);
        assert_eq!(std::fs::read(&unpacked.app_libraries[0].1).unwrap(), app_bytes);

        let bad_cases = [
            (
                "escape.tar",
                vec![SeedAppLibrary {
                    guest_path: "Applications/../escape.metallib".to_owned(),
                    path: app_rel.clone(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                }],
                true,
            ),
            (
                "non_app.tar",
                vec![SeedAppLibrary {
                    guest_path: "System/Library/foo.metallib".to_owned(),
                    path: app_rel.clone(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                }],
                true,
            ),
            (
                "bad_path.tar",
                vec![SeedAppLibrary {
                    guest_path: "Applications/SwitchHWTest.app/default.metallib".to_owned(),
                    path: "raw/own.metallib".to_owned(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                }],
                true,
            ),
            (
                "missing_file.tar",
                vec![SeedAppLibrary {
                    guest_path: "Applications/SwitchHWTest.app/default.metallib".to_owned(),
                    path: app_rel.clone(),
                    sha256: app_sha.clone(),
                    size: app_bytes.len() as u64,
                }],
                false,
            ),
            (
                "case_dup.tar",
                vec![
                    SeedAppLibrary {
                        guest_path: "Applications/SwitchHWTest.app/default.metallib".to_owned(),
                        path: app_rel.clone(),
                        sha256: app_sha.clone(),
                        size: app_bytes.len() as u64,
                    },
                    SeedAppLibrary {
                        guest_path: "Applications/switchhwtest.app/default.metallib".to_owned(),
                        path: app_rel.clone(),
                        sha256: app_sha.clone(),
                        size: app_bytes.len() as u64,
                    },
                ],
                true,
            ),
        ];
        for (idx, (name, libs, include_blob)) in bad_cases.into_iter().enumerate() {
            let bad_tar = make_tar(name, libs, include_blob);
            let err = unpack_seed_asset(
                &bad_tar,
                "24B119",
                &qc,
                Some(stack),
                &tmp.path().join(format!("u_bad_{idx}")),
            )
            .unwrap_err();
            assert!(matches!(err, SeedError::Asset(_)), "expected error for {name}");
        }
    }
}
