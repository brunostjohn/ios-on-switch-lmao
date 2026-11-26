// SPDX-License-Identifier: GPL-3.0-or-later
//! What switch_boot refuses for the selected kind: another build's record or bundle, a bundle or IPSW member that
//! differs from the one CI built from, another build's OS volume, and a launchd catalogue the record doesn't pin.

mod case;

use case::Case;
use switch_boot::{Error, boot_files};

fn refused(case: &Case, error: Error) -> Error {
    println!("{}: {error}", case.kind);
    error
}

#[test]
fn another_builds_record_is_refused() {
    let case = Case::load();
    let (assets, files) = (case.assets(), case.member_files());
    for other in case.other_kinds() {
        let error = boot_files(&case.record(&other), &assets, &files.members())
            .err()
            .expect("refused");
        assert!(matches!(refused(&case, error), Error::AssetsForAnotherBuild { .. }));
    }
}

#[test]
fn another_builds_bundle_is_refused() {
    let case = Case::load();
    let (mut assets, files) = (case.assets(), case.member_files());
    for other in case.other_kinds() {
        let build = case.record(&other).build;
        assets.insert(
            "boot.json",
            format!("{{\"build\": \"{build}\", \"format\": 1}}\n").into_bytes(),
        );
        let error = boot_files(&case.record(&case.kind), &assets, &files.members())
            .err()
            .expect("refused");
        assert!(matches!(refused(&case, error), Error::AssetsForAnotherBuild { .. }));
    }
}

#[test]
fn a_bundle_short_of_a_member_or_with_an_unknown_one_is_refused() {
    let case = Case::load();
    let (record, files) = (case.record(&case.kind), case.member_files());
    let mut assets = case.assets();
    assets.remove("bl33/u-boot.bin").expect("the bundle has U-Boot");
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    assert!(matches!(refused(&case, error), Error::Assets(ref m) if m.contains("bl33/u-boot.bin")));

    let mut assets = case.assets();
    assets.insert("kernel/kernel.macho.orig", Vec::new());
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    assert!(matches!(refused(&case, error), Error::Assets(ref m) if m.contains("kernel.macho.orig")));
}

/// One CDHash of the overlay module changed: the bundle, so the window, is no longer the one the loader was built for.
#[test]
fn a_changed_bundle_member_is_refused_by_the_loaders_constants() {
    let case = Case::load();
    let (record, files) = (case.record(&case.kind), case.member_files());
    let mut assets = case.assets();
    let mut module = assets.get("trust/overlay-base.trustcache").unwrap().to_vec();
    module[24] ^= 0x01;
    assets.insert("trust/overlay-base.trustcache", module);
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    let error = refused(&case, error);
    assert!(matches!(
        error,
        Error::Window(switch_window::Error::LoaderBuiltForAnotherWindow(_))
    ));
}

#[test]
fn a_changed_device_tree_is_refused_by_its_manifest_digest() {
    let case = Case::load();
    let (record, assets, mut files) = (case.record(&case.kind), case.assets(), case.member_files());
    let im4p = files.get_mut("device_tree");
    let at = im4p.len() / 2;
    im4p[at] ^= 0x01;
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    assert!(matches!(
        refused(&case, error),
        Error::Img4(switch_img4::Error::ManifestDigest)
    ));
}

/// The file offset of the arm64 slice's `__TEXT,__text`, from the Mach-O header.
fn text_section(installd: &[u8]) -> std::ops::Range<usize> {
    let slice = switch_installd::arm64_slice(installd).unwrap();
    let base = slice.as_ptr() as usize - installd.as_ptr() as usize;
    let u32_at = |at: usize| u32::from_le_bytes(slice[at..at + 4].try_into().unwrap()) as usize;
    let (count, mut at) = (u32_at(16), 32);
    for _ in 0..count {
        let (cmd, size) = (u32_at(at), u32_at(at + 4));
        if cmd == 0x19 && slice[at + 8..at + 24].starts_with(b"__TEXT\0") {
            for index in 0..u32_at(at + 64) {
                let section = at + 72 + index * 80;
                if slice[section..section + 16].starts_with(b"__text\0") {
                    let (offset, length) = (u32_at(section + 48), u32_at(section + 40));
                    return base + offset..base + offset + length;
                }
            }
        }
        at += size;
    }
    panic!("installd has no __TEXT,__text");
}

/// One instruction of installd changed: it is still patched and signed, but its CDHash is not the one CI trusted.
#[test]
fn a_changed_installd_is_refused_by_the_loaders_constants() {
    let case = Case::load();
    let (record, assets, mut files) = (case.record(&case.kind), case.assets(), case.member_files());
    let installd = files.get_mut("installd");
    let text = text_section(installd);
    installd[text.start + text.len() / 2] ^= 0x01;
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    let error = refused(&case, error);
    assert!(matches!(
        error,
        Error::Window(switch_window::Error::LoaderBuiltForAnotherWindow(_))
    ));
}

#[test]
fn another_builds_os_volume_is_refused_before_installd_is_signed() {
    let case = Case::load();
    let (record, assets, mut files) = (case.record(&case.kind), case.assets(), case.member_files());
    let version = files.get_mut("system_version");
    let mut plist: plist::Dictionary = plist::from_bytes(version).unwrap();
    plist.insert("ProductBuildVersion".into(), "99Z999".into());
    version.clear();
    plist::to_writer_xml(&mut *version, &plist).unwrap();
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    assert!(matches!(
        refused(&case, error),
        Error::Installd(switch_installd::Error::Build { .. })
    ));
}

#[test]
fn a_launchd_catalogue_other_than_the_records_is_refused() {
    let case = Case::load();
    let (record, assets, mut files) = (case.record(&case.kind), case.assets(), case.member_files());
    if record.launchd_catalog.is_some() {
        files.remove("launchd_catalog");
    } else {
        files.insert("launchd_catalog", b"bplist00".to_vec());
        files.insert("launchd_catalog_signature", Vec::new());
    }
    let error = boot_files(&record, &assets, &files.members()).err().expect("refused");
    assert!(matches!(refused(&case, error), Error::Members(_)));
}
