// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use clap::Parser;
use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, ENTRY_BYTES, Entry, SECTOR};
use switch_catalog::Catalog;
use switch_disk::{Bus, Disk, Partition};
use switch_engine::{
    CardProbe, EngineEvent, EngineStep, Mode, ProbeJournal, ProbeSize, ProbeSlot, StepOutcome, TargetSpec,
};
use switch_installer_lib::{
    Cli, CliMode, DiscoveredCard, GuardError, catalog_to_build_options, disk_confirm_token, disk_to_card_option,
    engine_event_to_tui, execute, format_dry_run, image_to_card_option, parse_size_bytes, validate_non_interactive,
};
use switch_tui::{ProgressEvent, SlotStatus, StepStatus};
use tempfile::TempDir;

const GIB: u64 = 1024 * 1024 * 1024;
const MIB: u64 = 1024 * 1024;

fn sample_catalog_json() -> serde_json::Value {
    let dummy32 = "00".repeat(32);
    let dummy48 = "00".repeat(48);
    let disk_m = |size: u64| {
        serde_json::json!({
            "member": "disk.dmg", "component": "OS", "offset": 0, "stored_size": size, "size": size,
            "sha256": dummy32, "content_sha256": dummy32, "deflated": false, "manifest_digest": dummy48,
            "aea": null, "container": { "format": "raw", "size": size, "sha256": dummy32 }, "im4p": null
        })
    };
    let im4p_m = |fourcc: &str| {
        serde_json::json!({
            "member": "im4p", "component": "Kernel", "offset": 0, "stored_size": 1024, "size": 1024,
            "sha256": dummy32, "content_sha256": dummy32, "deflated": false, "manifest_digest": dummy48,
            "aea": null, "container": null, "im4p": { "fourcc": fourcc, "payload_size": 512, "payload_sha256": dummy32 }
        })
    };
    let members = serde_json::json!({
        "os": disk_m(5 * GIB),
        "system_cryptex": disk_m(3 * GIB),
        "app_cryptex": disk_m(200 * MIB),
        "kernelcache": im4p_m("krnl"),
        "device_tree": im4p_m("dtre"),
        "static_trust_cache": im4p_m("trst"),
        "system_cryptex_trust_cache": im4p_m("rtsc"),
        "app_cryptex_trust_cache": im4p_m("trst"),
    });
    let volumes = serde_json::json!({
        "os_volume_uuid": "11111111-2222-4333-8444-555555555555",
        "system_cryptex_uuid": "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE",
        "system_cryptex_image": "/private/preboot/Switch-Staging/sys.dmg",
        "app_cryptex_uuid": "22222222-3333-4444-8555-666666666666",
        "app_cryptex_image": "/private/preboot/Switch-Staging/app.dmg",
    });
    serde_json::json!({
        "format": 1,
        "release": "v0.3.0",
        "builds": [
            {
                "build": "24B119",
                "version": "27.2",
                "ios": 27,
                "xnu": "12377.121.6",
                "product": "iPad12,1",
                "device_class": "j120ap",
                "ipsw": { "url": "https://example.invalid/24B119.ipsw", "size": 9 * GIB },
                "steps": { "prelink": "fileset", "boot_manifest_hash": "sha384" },
                "members": members,
                "volumes": volumes,
                "derived": {
                    "kernel_virt_base": "0xfffffe0007004000",
                    "quartzcore_metallib_sha256": dummy32,
                    "launchd_catalog_sha256": null,
                    "launchd_catalog_signature_sha256": null,
                    "pretranslated": {}
                },
                "assets": { "boot": { "name": "boot-24B119.tar", "size": 32 * MIB, "sha256": dummy32 } }
            },
            {
                "build": "25A999",
                "version": "28.0",
                "ios": 28,
                "xnu": "13000.0.0",
                "product": "iPad14,1",
                "device_class": "j999ap",
                "ipsw": { "url": "https://example.invalid/25A999.ipsw", "size": 10 * GIB },
                "steps": { "prelink": "fileset_v2", "future_step": "v3" },
                "members": members,
                "volumes": volumes,
                "derived": {
                    "kernel_virt_base": "0xfffffe0007004000",
                    "quartzcore_metallib_sha256": dummy32,
                    "launchd_catalog_sha256": null,
                    "launchd_catalog_signature_sha256": null,
                    "pretranslated": {}
                },
                "assets": {}
            }
        ]
    })
}

fn sample_catalog_with_supported_and_unsupported() -> Catalog {
    Catalog::parse(&serde_json::to_vec(&sample_catalog_json()).unwrap()).unwrap()
}

fn sample_disk(path: &str, model: &str) -> Disk {
    Disk {
        id: format!("id:{path}"),
        path: PathBuf::from(path),
        size: 256 * GIB,
        logical_sector: 512,
        physical_sector: 512,
        removable: true,
        internal: false,
        system: false,
        read_only: false,
        model: model.to_owned(),
        bus: Bus::Usb,
        partitions: vec![
            Partition {
                path: PathBuf::from(format!("{path}s1")),
                number: 1,
                offset: 16 * MIB,
                size: 180 * GIB,
                name: "hos_data".to_owned(),
                kind: "Microsoft Basic Data".to_owned(),
                volumes: Vec::new(),
            },
            Partition {
                path: PathBuf::from(format!("{path}s2")),
                number: 2,
                offset: 180 * GIB + 16 * MIB,
                size: 64 * GIB,
                name: "iOS".to_owned(),
                kind: "Apple_APFS".to_owned(),
                volumes: Vec::new(),
            },
        ],
        volumes: Vec::new(),
    }
}

#[test]
fn cli_parsing_and_size_units() {
    let cli = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "partition+install",
        "--build",
        "24B119",
        "--image",
        "/tmp/card.img",
        "--ios-size",
        "64G",
        "--yes",
        "--no-eject",
    ])
    .unwrap();
    assert_eq!(cli.mode, Some(CliMode::PartitionAndInstall));
    assert_eq!(cli.build.as_deref(), Some("24B119"));
    assert_eq!(cli.ios_size, Some(64 * GIB));
    assert!(cli.yes);
    assert!(cli.no_eject);
    assert!(!cli.should_use_tui(true));

    assert_eq!(parse_size_bytes("64G").unwrap(), 64 * GIB);
    assert_eq!(parse_size_bytes("64GiB").unwrap(), 64 * GIB);
    assert_eq!(parse_size_bytes("48.5G").unwrap(), 48 * GIB + 512 * MIB);
    assert_eq!(parse_size_bytes("16384M").unwrap(), 16 * GIB);
    assert_eq!(parse_size_bytes("33554432").unwrap(), 32 * MIB);
    assert!(parse_size_bytes("").is_err());
    assert!(parse_size_bytes("-4G").is_err());
}

#[test]
fn non_interactive_destructive_guards() {
    let missing_mode = Cli::try_parse_from(["switch_installer", "--card", "disk4", "--yes"]).unwrap();
    assert_eq!(
        validate_non_interactive(&missing_mode, Some("disk4"), true),
        Err(GuardError::MissingMode)
    );

    let missing_target = Cli::try_parse_from(["switch_installer", "--mode", "install", "--yes"]).unwrap();
    assert_eq!(
        validate_non_interactive(&missing_target, None, true),
        Err(GuardError::MissingTarget)
    );

    let no_yes = Cli::try_parse_from(["switch_installer", "--mode", "install", "--card", "disk4"]).unwrap();
    assert!(matches!(
        validate_non_interactive(&no_yes, Some("disk4"), true),
        Err(GuardError::MissingConfirmation { .. })
    ));

    let no_confirm =
        Cli::try_parse_from(["switch_installer", "--mode", "install", "--card", "disk4", "--yes"]).unwrap();
    assert!(matches!(
        validate_non_interactive(&no_confirm, Some("disk4"), true),
        Err(GuardError::MissingConfirmation { .. })
    ));

    let wrong_confirm = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "install",
        "--card",
        "disk4",
        "--yes",
        "--confirm",
        "disk5",
    ])
    .unwrap();
    assert!(matches!(
        validate_non_interactive(&wrong_confirm, Some("disk4"), true),
        Err(GuardError::ConfirmMismatch { .. })
    ));

    let ok_real_card = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "install",
        "--card",
        "disk4",
        "--yes",
        "--confirm",
        "disk4",
    ])
    .unwrap();
    assert_eq!(
        validate_non_interactive(&ok_real_card, Some("disk4"), true),
        Ok(Mode::Install)
    );

    let ok_image = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "partition+install",
        "--image",
        "/tmp/test.img",
        "--ios-size",
        "32G",
    ])
    .unwrap();
    assert_eq!(
        validate_non_interactive(&ok_image, None, true),
        Ok(Mode::PartitionAndInstall)
    );

    let bad_align = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "partition",
        "--image",
        "/tmp/test.img",
        "--ios-size",
        "100M",
    ])
    .unwrap();
    assert!(matches!(
        validate_non_interactive(&bad_align, None, true),
        Err(GuardError::UnalignedSize(_))
    ));
}

#[test]
fn build_options_dim_unsupported_catalog_records_with_reason() {
    let catalog = sample_catalog_with_supported_and_unsupported();
    let options = catalog_to_build_options(&catalog);
    assert_eq!(options.len(), 2);

    assert_eq!(options[0].id, "24B119");
    assert!(options[0].is_supported());
    assert!(options[0].min_partition_bytes.unwrap() >= 16 * GIB);

    assert_eq!(options[1].id, "25A999");
    assert!(!options[1].is_supported());
    assert!(options[1].min_partition_bytes.is_none());
    let reason = options[1].unsupported_reason.as_deref().unwrap();
    assert!(reason.contains("fileset_v2") || reason.contains("future_step"));
}

#[test]
fn card_option_mapping_confirm_tokens_slots_and_journals() {
    assert_eq!(disk_confirm_token(&sample_disk("/dev/disk4", "SD")), "disk4");
    assert_eq!(disk_confirm_token(&sample_disk("/dev/sdb", "SD")), "sdb");
    assert_eq!(
        disk_confirm_token(&sample_disk(r"\\.\PhysicalDrive2", "SD")),
        "PhysicalDrive2"
    );

    let disk = sample_disk("/dev/disk4", "SanDisk 256GB");
    let inferred = disk_to_card_option(&disk, None);
    assert_eq!(inferred.confirm_token, "disk4");
    assert!(matches!(
        inferred.slot,
        SlotStatus::ExistingIos { size_bytes, .. } if size_bytes == 64 * GIB
    ));
    assert!(inferred.size_constraints.is_some());

    let probe = CardProbe {
        capacity_bytes: 256 * GIB,
        mbr_only: false,
        layout_summary: "Interrupted partition journal".to_owned(),
        slot: ProbeSlot::None,
        pending_journal: Some(ProbeJournal {
            stage: "Relocating FAT32 tail clusters".to_owned(),
            detail: "Carving 64 GiB iOS-setup".to_owned(),
            entry_index: 1,
            ios_bytes: 64 * GIB,
        }),
        size: Some(ProbeSize {
            min_bytes: 16 * GIB,
            max_bytes: 120 * GIB,
            default_bytes: 64 * GIB,
            total_card_bytes: 256 * GIB,
            fat_total_bytes: 200 * GIB,
            fat_used_bytes: 80 * GIB,
            other_partitions_bytes: 32 * GIB,
        }),
        has_boot_volume: true,
        unusable_reason: None,
    };
    let card_opt = disk_to_card_option(&disk, Some(&probe));
    assert_eq!(
        card_opt.pending_journal.as_ref().unwrap().stage,
        "Relocating FAT32 tail clusters"
    );
    assert_eq!(card_opt.size_constraints.as_ref().unwrap().max_bytes, 120 * GIB);

    let img_opt = image_to_card_option(Path::new("/tmp/mycard.img"), &probe);
    assert_eq!(img_opt.confirm_token, "mycard.img");
    assert!(img_opt.pending_journal.is_some());
}

#[test]
fn dry_run_validates_image_and_catalog_without_mutating() {
    let tmp = TempDir::new().unwrap();
    let img = tmp.path().join("dryrun.img");
    write_minimal_gpt_image(&img, 400_000);
    let before = fs::read(&img).unwrap();

    let catalog = sample_catalog_with_supported_and_unsupported();
    let cat_path = tmp.path().join("catalog.json");
    fs::write(&cat_path, serde_json::to_vec(&sample_catalog_json()).unwrap()).unwrap();

    let cli = Cli::try_parse_from([
        "switch_installer",
        "--mode",
        "install",
        "--build",
        "24B119",
        "--image",
        img.to_str().unwrap(),
        "--catalog",
        cat_path.to_str().unwrap(),
        "--dry-run",
    ])
    .unwrap();

    assert!(execute(&cli, false).unwrap());
    assert_eq!(fs::read(&img).unwrap(), before, "dry-run must not modify image");

    let card = DiscoveredCard {
        option: disk_to_card_option(&sample_disk("/dev/disk4", "SD"), None),
        target: TargetSpec::image_file(&img),
        probe: None,
    };
    let text = format_dry_run(Mode::Install, &cli, Some(&catalog), &card).unwrap();
    assert!(text.contains("24B119"));
    assert!(text.contains("No writes performed (--dry-run)."));
}

#[test]
fn engine_events_map_to_tui_progress_events() {
    let steps = engine_event_to_tui(EngineEvent::SetSteps(vec![EngineStep::new("a", "Step A")]));
    assert!(matches!(steps, ProgressEvent::SetSteps(v) if v.len() == 1 && v[0].id == "a"));

    let fin = engine_event_to_tui(EngineEvent::StepFinished {
        step_id: "a".to_owned(),
        outcome: StepOutcome::Done,
        elapsed: Duration::from_secs(2),
    });
    assert!(matches!(
        fin,
        ProgressEvent::StepFinished {
            status: StepStatus::Done,
            ..
        }
    ));

    let irrev = engine_event_to_tui(EngineEvent::SetIrreversible(true));
    assert_eq!(irrev, ProgressEvent::SetIrreversible(true));
}

fn write_minimal_gpt_image(path: &Path, total_sectors: u64) {
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(path)
        .unwrap();
    file.set_len(total_sectors * SECTOR as u64).unwrap();

    let mut fat = Entry([0; ENTRY_BYTES]);
    fat.0[16..32].copy_from_slice(&[1; 16]);
    fat.0[32..40].copy_from_slice(&32_768u64.to_le_bytes());
    fat.0[40..48].copy_from_slice(&200_000u64.to_le_bytes());
    fat = fat.retyped(BASIC_DATA_TYPE, "hos_data");

    let mut ios = Entry([0; ENTRY_BYTES]);
    ios.0[16..32].copy_from_slice(&[2; 16]);
    ios.0[32..40].copy_from_slice(&200_001u64.to_le_bytes());
    ios.0[40..48].copy_from_slice(&350_000u64.to_le_bytes());
    ios = ios.retyped(APFS_TYPE, "iOS");

    let mut mbr = [0u8; SECTOR];
    mbr[450] = 0x0c;
    mbr[454..458].copy_from_slice(&32_768u32.to_le_bytes());
    mbr[458..462].copy_from_slice(&(200_000 - 32_768 + 1u32).to_le_bytes());
    mbr[466] = 0xee;
    mbr[470..474].copy_from_slice(&1u32.to_le_bytes());
    mbr[474..478].copy_from_slice(&((total_sectors - 1) as u32).to_le_bytes());
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    file.write_all(&mbr).unwrap();

    let mut array = vec![0u8; 32 * SECTOR];
    array[..ENTRY_BYTES].copy_from_slice(&fat.0);
    array[ENTRY_BYTES..2 * ENTRY_BYTES].copy_from_slice(&ios.0);
    let crc = crc32fast::hash(&array);

    let hdr = |self_lba: u64, alt_lba: u64, entry_lba: u64| {
        let mut h = [0u8; SECTOR];
        h[..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&self_lba.to_le_bytes());
        h[32..40].copy_from_slice(&alt_lba.to_le_bytes());
        h[40..48].copy_from_slice(&34u64.to_le_bytes());
        h[48..56].copy_from_slice(&(total_sectors - 34).to_le_bytes());
        h[56..72].copy_from_slice(b"DryRunCardTest01");
        h[72..80].copy_from_slice(&entry_lba.to_le_bytes());
        h[80..84].copy_from_slice(&128u32.to_le_bytes());
        h[84..88].copy_from_slice(&128u32.to_le_bytes());
        h[88..92].copy_from_slice(&crc.to_le_bytes());
        let hc = crc32fast::hash(&h[..92]);
        h[16..20].copy_from_slice(&hc.to_le_bytes());
        h
    };

    file.seek(SeekFrom::Start(SECTOR as u64)).unwrap();
    file.write_all(&hdr(1, total_sectors - 1, 2)).unwrap();
    file.write_all(&array).unwrap();
    file.seek(SeekFrom::Start((total_sectors - 33) * SECTOR as u64))
        .unwrap();
    file.write_all(&array).unwrap();
    file.write_all(&hdr(total_sectors - 1, 1, total_sectors - 33)).unwrap();
    file.sync_data().unwrap();
}
