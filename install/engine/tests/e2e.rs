// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::container::{NX_MAGIC, NxSuperblock};
use switch_apfs_format::io::FileSource;
use switch_apfs_format::object::{OBJ_PHYSICAL, ObjPhys, TYPE_NX_SUPERBLOCK};
use switch_apfs_format::{put, seal};
use switch_apfs_read::{Container, Fs, stream};
use switch_card::{APFS_TYPE, BASIC_DATA_TYPE, Card, DEMOTED_NAME, LINUX_TYPE, ROOT_NAME, SECTOR, SETUP_NAME};
use switch_engine::{
    CancelToken, DATA_VOLUME_UUID_STR, Mode, NoopSink, OpenedTarget, PREBOOT_VOLUME_UUID_STR, ResumeStore, RunRequest,
    TargetSpec, partition_fingerprint, read_pending_journal, run,
};
use switch_image::{DEFAULT_DATA_NAME, DEFAULT_PREBOOT_NAME, parse_uuid};
use tempfile::TempDir;
use zerocopy::FromZeros;
use zerocopy::little_endian::{U32, U64};

use crate::synth_cryptex::verify_sealed_cryptex_bytes;
use crate::synth_fat::{
    FAT_FIRST_LBA, create_gpt_card_file, format_fat32_region, make_gpt_entry, read_fat_file, write_fat_file,
};
use crate::synth_fixture::build_synthetic_fixture;

#[test]
fn e2e_partition_mode_on_synthetic_fat32_image() {
    let tmp = TempDir::new().unwrap();
    let img = tmp.path().join("card.img");
    let fat_sectors: u64 = 41_943_040;
    let total_sectors: u64 = FAT_FIRST_LBA + fat_sectors + 4096;
    let fat_entry = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_entry)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 64);

    write_fat_file(
        &img,
        FAT_FIRST_LBA,
        fat_sectors,
        "Nintendo/save.bin",
        b"horizon-save-data",
    );
    write_fat_file(
        &img,
        FAT_FIRST_LBA,
        fat_sectors,
        "switchroot/ios/bootargs.txt",
        b"serial=1\n",
    );

    let mut req = RunRequest::new(Mode::Partition, TargetSpec::image_file(&img));
    req.ios_size_bytes = Some(16 << 30);
    req.work_dir = Some(tmp.path().join("work"));
    req.log_path = Some(tmp.path().join("part.log"));

    let mut sink = NoopSink;
    let summary = run(&req, None, &mut sink, CancelToken::new()).unwrap();
    assert_eq!(summary.mode, Mode::Partition);
    assert!(!summary.resumed_journal);
    assert_eq!(summary.partition_index, Some(1));
    assert_eq!(summary.partition_bytes, Some(16 << 30));

    let mut dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    assert!(read_pending_journal(&mut dev).unwrap().is_none());
    let card = Card::open(&mut dev).unwrap();
    assert!(!card.torn());
    let e1 = card.entries()[1];
    assert!(e1.named(SETUP_NAME));
    assert_eq!(e1.type_guid(), LINUX_TYPE);
    assert_eq!(e1.sectors() * SECTOR as u64, 16 << 30);

    let (fat_first, fat_new) = card.boot_volume().unwrap();
    assert_eq!(
        read_fat_file(&img, fat_first, fat_new, "Nintendo/save.bin").as_deref(),
        Some(&b"horizon-save-data"[..])
    );
    assert_eq!(
        read_fat_file(&img, fat_first, fat_new, "switchroot/ios/bootargs.txt").as_deref(),
        Some(&b"serial=1\n"[..])
    );
}

#[test]
fn e2e_install_mode_sealed_cryptex_overlay_bootstrap_and_demotion() {
    let fixture = build_synthetic_fixture(true);
    let img = fixture.tmp.path().join("install-card.img");

    let fat_sectors: u64 = 160_000;
    let old_ios_first = FAT_FIRST_LBA + fat_sectors;
    let old_ios_sectors: u64 = 65_536;
    let target_first = old_ios_first + old_ios_sectors;
    let target_sectors: u64 = 4_194_304;
    let total_sectors = target_first + target_sectors + 4096;

    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    let old_e = make_gpt_entry(APFS_TYPE, 2, ROOT_NAME, old_ios_first, old_ios_sectors);
    let target_e = make_gpt_entry(LINUX_TYPE, 3, SETUP_NAME, target_first, target_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e), (1, old_e), (2, target_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 1);

    write_fat_file(
        &img,
        FAT_FIRST_LBA,
        fat_sectors,
        "switchroot/ios/bootargs.txt",
        b"keep-me=1\n",
    );
    write_fat_file(
        &img,
        FAT_FIRST_LBA,
        fat_sectors,
        "switchroot/ios/stale.bin",
        b"remove-me",
    );

    let mut old_nx = NxSuperblock::new_zeroed();
    old_nx.o = ObjPhys::new(1, 7, OBJ_PHYSICAL | TYPE_NX_SUPERBLOCK, 0);
    old_nx.magic = U32::new(NX_MAGIC);
    old_nx.block_size = U32::new(BLOCK_SIZE as u32);
    old_nx.block_count = U64::new(4096);
    old_nx.xp_desc_base = U64::new(80);
    old_nx.xp_desc_blocks = U32::new(4);
    let mut nx_blk = [0u8; BLOCK_SIZE];
    put(&mut nx_blk, 0, &old_nx);
    seal(&mut nx_blk);
    {
        let dev = OpenedTarget::open(&TargetSpec::image_file(&img)).unwrap();
        let offset = target_first * SECTOR as u64;
        dev.write_at(offset, &nx_blk).unwrap();
        dev.write_at(offset + 80 * BLOCK_SIZE as u64, &nx_blk).unwrap();
        dev.flush().unwrap();
    }

    let mut req = RunRequest::new(Mode::Install, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut req, true);

    let mut sink = NoopSink;
    let summary = run(&req, None, &mut sink, CancelToken::new()).unwrap();
    assert_eq!(summary.mode, Mode::Install);
    assert_eq!(summary.build.as_deref(), Some("24B119"));
    assert_eq!(summary.partition_index, Some(2));
    assert_eq!(summary.boot_files_written, 3);
    assert!(fixture.log_path.is_file());

    let fp = partition_fingerprint(total_sectors, 2, target_first, target_sectors, &target_e.unique_guid());
    assert!(
        ResumeStore::new(&fixture.work_dir)
            .load(&fp, target_first, target_sectors)
            .unwrap()
            .is_none()
    );

    let mut dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    let card = Card::open(&mut dev).unwrap();
    assert!(!card.torn());
    assert_eq!(card.roots(), vec![2]);
    assert!(card.entries()[2].named(ROOT_NAME));
    assert_eq!(card.entries()[2].type_guid(), APFS_TYPE);
    assert!(card.entries()[1].named(DEMOTED_NAME));
    assert_eq!(card.entries()[1].type_guid(), LINUX_TYPE);

    assert_eq!(
        read_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "switchroot/ios/bl33.bin").as_deref(),
        Some(&b"BL33-PAYLOAD"[..])
    );
    assert_eq!(
        read_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "bootloader/ini/ios.ini").as_deref(),
        Some(&b"[iOS]\npayload=1\n"[..])
    );
    assert_eq!(
        read_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "bootloader/res/icon_ios_hue.bmp").as_deref(),
        Some(&b"BMICON"[..])
    );
    assert_eq!(
        read_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "switchroot/ios/bootargs.txt").as_deref(),
        Some(&b"keep-me=1\n"[..])
    );
    assert!(read_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "switchroot/ios/stale.bin").is_none());

    let part_offset = target_first * SECTOR as u64;
    let src = FileSource::open(&img, part_offset).unwrap();
    let container = Container::open(&src).unwrap();
    let audit = switch_apfs_read::audit(&container, &src).unwrap();
    assert!(audit.ok(), "APFS container audit failed:\n{audit}");
    assert_eq!(container.volumes.len(), 3);

    let data_vol = container.volume(parse_uuid(DATA_VOLUME_UUID_STR).unwrap()).unwrap();
    assert_eq!(data_vol.sb.name(), DEFAULT_DATA_NAME);
    assert_eq!(data_vol.sb.readonly_compatible_features.get(), 0x0c);
    assert_eq!(data_vol.sb.incompatible_features.get(), 0x208);

    let data_fs = Fs::open(&src, data_vol).unwrap();
    assert!(data_fs.lookup("/Switch/overlay/etc/switch.conf").unwrap().is_some());
    assert!(
        data_fs
            .lookup("/Switch/overlay/usr/libexec/installd")
            .unwrap()
            .is_some()
    );
    assert!(data_fs.lookup("/Switch/pretranslated/.index").unwrap().is_some());
    assert!(data_fs.lookup("/Switch/pretranslated.json").unwrap().is_some());
    assert!(
        data_fs
            .lookup("/Switch/bootstrap/24B119/manifest.json")
            .unwrap()
            .is_some()
    );
    assert!(data_fs.lookup("/jb").unwrap().is_some());
    assert!(data_fs.lookup("/Switch/gpu/driver.meta").unwrap().is_some());
    assert!(data_fs.lookup("/Switch/seed/shaders.bin").unwrap().is_some());

    let preboot_vol = container.volume(parse_uuid(PREBOOT_VOLUME_UUID_STR).unwrap()).unwrap();
    assert_eq!(preboot_vol.sb.name(), DEFAULT_PREBOOT_NAME);
    let preboot_fs = Fs::open(&src, preboot_vol).unwrap();

    for (path, expected_name, expected_body) in [
        (
            "/Switch-Staging/sys-test.dmg",
            "/sys_bin",
            &b"translated-sys-cryptex-payload-v2"[..],
        ),
        (
            "/Switch-Staging/app-test.dmg",
            "/app_bin",
            &b"translated-app-cryptex-payload-v2"[..],
        ),
    ] {
        let oid = preboot_fs.lookup(path).unwrap().expect("cryptex staged in Preboot");
        let (entry, _) = preboot_fs.entry(oid, path.to_owned(), None).unwrap();
        let data_stream = entry.data.as_ref().expect("cryptex has data stream");
        assert_eq!(
            data_stream.extents.len(),
            1,
            "{path} must occupy a single contiguous extent"
        );
        let mut raw = Vec::with_capacity(data_stream.size as usize);
        stream::read(&src, data_stream, &mut |chunk| raw.extend_from_slice(chunk)).unwrap();
        let files = verify_sealed_cryptex_bytes(&raw);
        assert_eq!(files, vec![(expected_name.to_owned(), expected_body.to_vec())]);
    }
}

#[test]
fn e2e_partition_and_install_single_pass() {
    let fixture = build_synthetic_fixture(false);
    let img = fixture.tmp.path().join("part-install.img");

    let fat_sectors: u64 = 41_943_040;
    let total_sectors = FAT_FIRST_LBA + fat_sectors + 4096;
    let fat_e = make_gpt_entry(BASIC_DATA_TYPE, 1, "hos_data", FAT_FIRST_LBA, fat_sectors);
    create_gpt_card_file(&img, total_sectors, &[(0, fat_e)]);
    format_fat32_region(&img, FAT_FIRST_LBA, fat_sectors, 64);

    write_fat_file(&img, FAT_FIRST_LBA, fat_sectors, "Horizon/user.dat", b"user-data-ok");

    let mut req = RunRequest::new(Mode::PartitionAndInstall, TargetSpec::image_file(&img));
    fixture.apply_to_request(&mut req, false);
    req.ios_size_bytes = Some(16 << 30);

    let mut sink = NoopSink;
    let summary = run(&req, None, &mut sink, CancelToken::new()).unwrap();
    assert_eq!(summary.mode, Mode::PartitionAndInstall);
    assert_eq!(summary.partition_index, Some(1));
    assert_eq!(summary.partition_bytes, Some(16 << 30));
    assert_eq!(summary.build.as_deref(), Some("24B119"));

    let mut dev = OpenedTarget::open_image_readonly(&img, SECTOR as u32).unwrap();
    assert!(read_pending_journal(&mut dev).unwrap().is_none());
    let card = Card::open(&mut dev).unwrap();
    assert_eq!(card.roots(), vec![1]);
    assert!(card.entries()[1].named(ROOT_NAME));
    assert_eq!(card.entries()[1].type_guid(), APFS_TYPE);

    let (fat_first, fat_new) = card.boot_volume().unwrap();
    assert_eq!(
        read_fat_file(&img, fat_first, fat_new, "Horizon/user.dat").as_deref(),
        Some(&b"user-data-ok"[..])
    );
    assert_eq!(
        read_fat_file(&img, fat_first, fat_new, "switchroot/ios/bl33.bin").as_deref(),
        Some(&b"BL33-PAYLOAD"[..])
    );

    let part_offset = card.entries()[1].first() * SECTOR as u64;
    let src = FileSource::open(&img, part_offset).unwrap();
    let container = Container::open(&src).unwrap();
    let audit = switch_apfs_read::audit(&container, &src).unwrap();
    assert!(audit.ok(), "partition+install APFS audit failed:\n{audit}");
    assert_eq!(container.volumes.len(), 3);
}
