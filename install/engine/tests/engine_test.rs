// SPDX-License-Identifier: GPL-3.0-or-later
mod e2e;
mod synth_apfs;
mod synth_cryptex;
mod synth_fat;
mod synth_fixture;

use std::fs;
use std::time::Duration;

use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::container::{NX_MAGIC, NxSuperblock};
use switch_apfs_format::io::FileSource;
use switch_apfs_format::object::{OBJ_PHYSICAL, ObjPhys, TYPE_NX_SUPERBLOCK};
use switch_apfs_format::{put, seal};
use switch_apfs_read::Container;
use switch_card::{BASIC_DATA_TYPE, Card, LINUX_TYPE, ROOT_NAME, SECTOR, SETUP_NAME};
use switch_engine::{
    CancelToken, EngineError, EngineEvent, HTTP_CONNECT_TIMEOUT, HTTP_READ_TIMEOUT, Mode, NoopSink, OpenedTarget,
    ResumeState, ResumeStore, RunRequest, TargetSpec, build_http_client, partition_fingerprint, read_pending_journal,
    run, run_detailed,
};
use switch_partition::journal;
use tempfile::TempDir;
use zerocopy::FromZeros;
use zerocopy::little_endian::{U32, U64};

use crate::synth_apfs::sha256_file;
use crate::synth_fat::{FAT_FIRST_LBA, create_gpt_card_file, format_fat32_region, make_gpt_entry};
use crate::synth_fixture::build_synthetic_fixture;

#[test]
fn resume_state_roundtrip_stale_areas_and_invalidation() {
    let tmp = TempDir::new().unwrap();
    let store = ResumeStore::new(tmp.path());
    let fp = partition_fingerprint(1_000_000, 2, 100_000, 500_000, &[0x42; 16]);

    let mut state = ResumeState::new(fp.clone(), "24B119".to_owned(), 100_000, 500_000, &[100..108, 200..210]);
    state.phase_b_started = true;
    state.mark_step("download", "sha-abc");
    store.save(&state).unwrap();

    let loaded = store.load(&fp, 100_000, 500_000).unwrap().expect("resume state loads");
    assert_eq!(loaded, state);
    assert_eq!(loaded.stale_ranges(), vec![100..108, 200..210]);
    assert!(loaded.step_matches("download", "sha-abc"));

    assert!(store.load(&fp, 100_001, 500_000).unwrap().is_none());
    assert!(store.load(&fp, 100_000, 500_001).unwrap().is_none());
    let other_fp = partition_fingerprint(1_000_000, 2, 100_000, 500_000, &[0x99; 16]);
    assert!(store.load(&other_fp, 100_000, 500_000).unwrap().is_none());

    let mut reconciled = loaded;
    reconciled.reconcile_build("21H461");
    assert_eq!(reconciled.build, "21H461");
    assert!(!reconciled.step_matches("download", "sha-abc"));
    assert_eq!(reconciled.stale_ranges(), vec![100..108, 200..210]);
    assert!(reconciled.phase_b_started);

    store.clear(&fp).unwrap();
    assert!(store.load(&fp, 100_000, 500_000).unwrap().is_none());
}

#[test]
fn rerun_uses_persisted_stale_descriptor_area_after_block_zero_wiped() {
    let fixture = build_synthetic_fixture(false);
    let img = fixture.tmp.path().join("rerun-stale.img");
    let fat_sectors: u64 = 160_000;
    let target_first = FAT_FIRST_LBA + fat_sectors;
    let target_sectors: u64 = 4_194_304;
    let total_sectors = target_first + target_sectors + 4096;

    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    let target_e = make_gpt_entry(LINUX_TYPE, 2, SETUP_NAME, target_first, target_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e), (1, target_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 1);

    // Simulate a torn phase B: block 0 is already zeroed, leaving an old NXSB at block 5000.
    let stale_block = 5000u64;
    let mut old_nx = NxSuperblock::new_zeroed();
    old_nx.o = ObjPhys::new(1, 99, OBJ_PHYSICAL | TYPE_NX_SUPERBLOCK, 0);
    old_nx.magic = U32::new(NX_MAGIC);
    old_nx.block_size = U32::new(BLOCK_SIZE as u32);
    old_nx.block_count = U64::new(8192);
    let mut nx_blk = [0u8; BLOCK_SIZE];
    put(&mut nx_blk, 0, &old_nx);
    seal(&mut nx_blk);
    {
        let dev = OpenedTarget::open(&TargetSpec::image_file(&img)).unwrap();
        dev.write_at(target_first * SECTOR as u64 + stale_block * BLOCK_SIZE as u64, &nx_blk)
            .unwrap();
        dev.flush().unwrap();
    }

    let fp = partition_fingerprint(total_sectors, 1, target_first, target_sectors, &target_e.unique_guid());
    let mut state = ResumeState::new(
        fp.clone(),
        "24B119".to_owned(),
        target_first,
        target_sectors,
        std::slice::from_ref(&(stale_block..stale_block + 4)),
    );
    state.phase_b_started = true;
    ResumeStore::new(&fixture.work_dir).save(&state).unwrap();

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut req, false);
    let mut sink = NoopSink;
    run(&req, None, &mut sink, CancelToken::new()).unwrap();

    let dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    let mut after = [0u8; BLOCK_SIZE];
    dev.read_at(
        target_first * SECTOR as u64 + stale_block * BLOCK_SIZE as u64,
        &mut after,
    )
    .unwrap();
    assert!(!switch_apfs_write::target::is_nxsb(&after));
}

#[test]
fn cancel_before_irreversible_write_leaves_target_untouched() {
    let fixture = build_synthetic_fixture(false);
    let img = fixture.tmp.path().join("cancel-before.img");
    let fat_sectors: u64 = 160_000;
    let target_first = FAT_FIRST_LBA + fat_sectors;
    let target_sectors: u64 = 262_144;
    let total_sectors = target_first + target_sectors + 4096;

    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    let target_e = make_gpt_entry(LINUX_TYPE, 2, SETUP_NAME, target_first, target_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e), (1, target_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 1);

    let before_digest = sha256_file(&img);
    let cancel = CancelToken::new();
    let cancel_trigger = cancel.clone();
    let mut sink = move |ev: EngineEvent| {
        if let EngineEvent::StepStarted { step_id, .. } = ev
            && step_id == "assemble"
        {
            cancel_trigger.cancel();
        }
    };

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut req, false);
    let failure = run_detailed(&req, None, &mut sink, cancel).unwrap_err();
    assert!(matches!(*failure.error, EngineError::Cancelled));
    assert!(!failure.irreversible);
    assert_eq!(
        sha256_file(&img),
        before_digest,
        "target must not be mutated before irreversible writes"
    );
}

#[test]
fn cancel_after_irreversible_write_completes_safely() {
    let fixture = build_synthetic_fixture(false);
    let img = fixture.tmp.path().join("cancel-after.img");
    let fat_sectors: u64 = 160_000;
    let target_first = FAT_FIRST_LBA + fat_sectors;
    let target_sectors: u64 = 4_194_304;
    let total_sectors = target_first + target_sectors + 4096;

    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    let target_e = make_gpt_entry(LINUX_TYPE, 2, SETUP_NAME, target_first, target_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e), (1, target_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 1);

    let cancel = CancelToken::new();
    let cancel_trigger = cancel.clone();
    let mut sink = move |ev: EngineEvent| {
        if let EngineEvent::StepStarted {
            step_id,
            irreversible: true,
        } = ev
            && step_id == "write_apfs"
        {
            cancel_trigger.cancel();
        }
    };

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut req, false);
    let summary = run(&req, None, &mut sink, cancel).expect("irreversible write must finish safely");
    assert_eq!(summary.partition_index, Some(1));

    let mut dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    let card = Card::open(&mut dev).unwrap();
    assert!(card.entries()[1].named(ROOT_NAME));
    let src = FileSource::open(&img, target_first * SECTOR as u64).unwrap();
    let audit = switch_apfs_read::audit(&Container::open(&src).unwrap(), &src).unwrap();
    assert!(audit.ok(), "audit failed:\n{audit}");
}

#[test]
fn install_enforces_pending_journal_resume_before_card_open() {
    let fixture = build_synthetic_fixture(false);
    let img = fixture.tmp.path().join("pending-journal.img");
    let fat_sectors: u64 = 41_943_040;
    let total_sectors = FAT_FIRST_LBA + fat_sectors + 4096;
    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 64);

    {
        let mut dev = OpenedTarget::open(&TargetSpec::image_file(&img)).unwrap();
        let survey = switch_partition::survey(&mut dev).unwrap();
        let limits = survey.limits(0);
        let plan = survey.plan(16 << 30, limits.min).unwrap();
        journal::begin(&mut dev, &plan).unwrap();
    }

    let mut install_req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut install_req, false);
    install_req.resume_journal = false;
    let mut sink = NoopSink;
    let err = run(&install_req, None, &mut sink, CancelToken::new()).unwrap_err();
    assert!(
        matches!(err, EngineError::PendingJournal { .. }),
        "expected PendingJournal refusal, got {err:?}"
    );

    let mut part_req = RunRequest::new(Mode::Partition, TargetSpec::image_file(&img));
    part_req.work_dir = Some(fixture.work_dir.clone());
    part_req.log_path = Some(fixture.log_path.clone());
    let summary = run(&part_req, None, &mut sink, CancelToken::new()).unwrap();
    assert!(summary.resumed_journal);
    assert_eq!(summary.partition_index, Some(1));

    let mut dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    assert!(read_pending_journal(&mut dev).unwrap().is_none());
    let card = Card::open(&mut dev).unwrap();
    assert!(card.entries()[1].named(SETUP_NAME));
}

#[test]
fn http_client_and_unsupported_catalog_checks() {
    assert_eq!(HTTP_CONNECT_TIMEOUT, Duration::from_secs(30));
    assert_eq!(HTTP_READ_TIMEOUT, Duration::from_secs(60));
    build_http_client().expect("reqwest client builds with timeouts");

    let tmp = TempDir::new().unwrap();
    let cat_path = tmp.path().join("bad-step-catalog.json");
    let dummy32 = "00".repeat(32);
    let dummy48 = "00".repeat(48);
    let member = serde_json::json!({
        "member": "x", "component": "x", "offset": 0, "stored_size": 1, "size": 1,
        "sha256": dummy32, "content_sha256": dummy32, "deflated": false, "manifest_digest": dummy48,
        "aea": null, "container": { "format": "raw", "size": 1, "sha256": dummy32 }, "im4p": null
    });
    let im4p = serde_json::json!({
        "member": "x", "component": "x", "offset": 0, "stored_size": 1, "size": 1,
        "sha256": dummy32, "content_sha256": dummy32, "deflated": false, "manifest_digest": dummy48,
        "aea": null, "container": null, "im4p": { "fourcc": "krnl", "payload_size": 1, "payload_sha256": dummy32 }
    });
    let json = serde_json::json!({
        "format": 1,
        "release": "v9.9.9",
        "builds": [{
            "build": "99Z999",
            "version": "99.0",
            "ios": 99,
            "xnu": "99999.0.0",
            "product": "iPad99,1",
            "device_class": "j999ap",
            "ipsw": { "url": "https://example.invalid/x.ipsw", "size": 1 },
            "steps": { "prelink": "future_v9" },
            "members": {
                "os": member, "system_cryptex": member, "app_cryptex": member,
                "kernelcache": im4p, "device_tree": im4p,
                "static_trust_cache": im4p, "system_cryptex_trust_cache": im4p, "app_cryptex_trust_cache": im4p
            },
            "volumes": {
                "os_volume_uuid": "11111111-2222-4333-8444-555555555555",
                "system_cryptex_uuid": "11111111-2222-4333-8444-555555555555",
                "system_cryptex_image": "/private/preboot/Switch-Staging/s.dmg",
                "app_cryptex_uuid": "11111111-2222-4333-8444-555555555555",
                "app_cryptex_image": "/private/preboot/Switch-Staging/a.dmg"
            },
            "derived": {
                "kernel_virt_base": "0xfffffe0007004000",
                "quartzcore_metallib_sha256": dummy32,
                "launchd_catalog_sha256": null,
                "launchd_catalog_signature_sha256": null,
                "pretranslated": {}
            },
            "assets": {}
        }]
    });
    fs::write(&cat_path, serde_json::to_vec(&json).unwrap()).unwrap();
    let catalog = switch_engine::load_catalog(Some(&cat_path), None, None).unwrap();
    let err = switch_engine::select_build(&catalog, Some("99Z999")).unwrap_err();
    assert!(matches!(err, EngineError::UnsupportedBuild(_)));
}
