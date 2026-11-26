// SPDX-License-Identifier: GPL-3.0-or-later
//! Each of `//boot:package`'s steps that the installer runs, on the record's data, the bundle's files and the IPSW's.

use switch_adt::{Finish, TrustModules};
use switch_prelink::{FilesetInputs, Inputs17, load_sites, prelink_17, prelink_fileset};
use switch_window::{Boot, Bundle, Inputs, Kind};

use crate::assets::{self as member, Assets};
use crate::record::{Prelink, Record, digest};
use crate::{Error, LaunchdCatalog, Members};

/// The IPSW's IM4P payloads but the kernelcache, each checked against the build manifest's digest of its IM4P.
pub struct Apple {
    pub device_tree: Vec<u8>,
    pub static_trust: Vec<u8>,
    pub system_trust: Vec<u8>,
    pub app_trust: Vec<u8>,
}

pub fn apple(record: &Record, members: &Members) -> Result<Apple, Error> {
    let digests = &record.manifest_digests;
    let payload = |im4p: &[u8], fourcc: &[u8; 4], what: &str, hex: &str| -> Result<Vec<u8>, Error> {
        Ok(switch_img4::payload(im4p, *fourcc, Some(&digest::<48>(what, hex)?))?)
    };
    Ok(Apple {
        device_tree: payload(members.device_tree, b"dtre", "device_tree", &digests.device_tree)?,
        static_trust: payload(
            members.static_trust_cache,
            b"trst",
            "static_trust_cache",
            &digests.static_trust_cache,
        )?,
        system_trust: payload(
            members.system_cryptex_trust_cache,
            b"trcs",
            "system_cryptex_trust_cache",
            &digests.system_cryptex_trust_cache,
        )?,
        app_trust: payload(
            members.app_cryptex_trust_cache,
            b"trca",
            "app_cryptex_trust_cache",
            &digests.app_cryptex_trust_cache,
        )?,
    })
}

/// The kernelcache's kexts prelinked into the release's kernel, the way the record names.
pub fn prelink(record: &Record, assets: &Assets, kernelcache_im4p: &[u8]) -> Result<Vec<u8>, Error> {
    let pin = digest::<32>("kernelcache_sha256", &record.kernelcache_sha256)?;
    let kernelcache = switch_img4::kernelcache(kernelcache_im4p, &pin)?;
    let kernel = assets.file(member::KERNEL)?;
    let sites: Vec<&[u8]> = assets.under(member::SITES).map(|(_, table)| table).collect();
    Ok(match record.prelink {
        Prelink::Native => {
            let inputs = Inputs17::new(
                assets.file(member::PRELINK_OPTIONS)?,
                assets.file(member::PRELINK_PREVIOUS)?,
                assets.file(member::PRELINK_INHERITED)?,
                load_sites(&sites, &record.build)?,
            )?;
            prelink_17(kernel, &kernelcache, &inputs)?
        }
        Prelink::Fileset => {
            let inputs = FilesetInputs::new(
                assets.file(member::BINDINGS)?,
                assets.file(member::FILESET_PINS)?,
                &sites,
            )?;
            prelink_fileset(kernel, &kernelcache, &inputs)?
        }
    })
}

/// Apple's launchd catalogue with the project's jobs, for a build whose record pins the catalogue.
pub fn launchd(
    record: &Record,
    assets: &Assets,
    given: Option<&LaunchdCatalog>,
) -> Result<Option<switch_launchd::Catalog>, Error> {
    match (&record.launchd_catalog, given) {
        (None, None) => Ok(None),
        (Some(pins), Some(given)) => {
            let jobs = switch_launchd::Jobs::parse(
                assets.file(member::BOOTSTRAP_JOB)?,
                assets.file(member::APPS_JOB)?,
                assets.file(member::SDWAKE_JOB)?,
            )?;
            let pins = switch_launchd::Pins {
                catalog_sha256: &pins.sha256,
                signature_sha256: &pins.signature_sha256,
            };
            Ok(Some(switch_launchd::build(given.plist, given.signature, &pins, &jobs)?))
        }
        (Some(_), None) => Err(Error::Members(
            "the record pins Apple's launchd catalogue, and none is given",
        )),
        (None, Some(_)) => Err(Error::Members(
            "a launchd catalogue is given for a build that pins none",
        )),
    }
}

/// The release's base ADT given the IPSW's iPad identity and the build's /chosen entries (adt_edit.py finish).
pub fn adt(
    record: &Record,
    assets: &Assets,
    apple: &Apple,
    launchd: Option<&switch_launchd::Catalog>,
) -> Result<Vec<u8>, Error> {
    let os_cryptex: Vec<&[u8]> = std::iter::once(apple.system_trust.as_slice())
        .chain(assets.get(member::PRETRANSLATED_SYSTEM_CRYPTEX))
        .collect();
    let app_cryptex: Vec<&[u8]> = std::iter::once(apple.app_trust.as_slice())
        .chain(assets.get(member::PRETRANSLATED_APP_CRYPTEX))
        .collect();
    let pretranslated: Vec<&[u8]> = assets.get(member::PRETRANSLATED_OS).into_iter().collect();
    let finish = Finish {
        root_volume_uuid: &record.os_volume_uuid,
        boot_manifest_hash: record.boot_manifest_hash,
        launchd_catalog_sha256: launchd.map(|c| &c.digest),
        a64x_cache_pretranslated: assets.get(member::PRETRANSLATED_CACHE_LIST),
        trust_modules: TrustModules {
            os_cryptex: &os_cryptex,
            app_cryptex: &app_cryptex,
            pretranslated: &pretranslated,
        },
    };
    Ok(switch_adt::finish(
        assets.file(member::ADT_BASE)?,
        &apple.device_tree,
        &finish,
    )?)
}

/// Apple's static trust cache with every other module merged in, `installd`'s CDHash among them.
pub fn trust(
    assets: &Assets,
    apple: &Apple,
    launchd: Option<&switch_launchd::Catalog>,
    installd: [u8; 20],
) -> Result<Vec<u8>, Error> {
    let modules: Vec<&[u8]> = [apple.system_trust.as_slice(), apple.app_trust.as_slice()]
        .into_iter()
        .chain([assets.file(member::OVERLAY_TRUST)?])
        .chain(launchd.map(|c| c.trust.as_slice()))
        .chain(assets.get(member::GPU_TRUST))
        .chain(
            [
                member::PRETRANSLATED_OS,
                member::PRETRANSLATED_APP_CRYPTEX,
                member::PRETRANSLATED_SYSTEM_CRYPTEX,
            ]
            .into_iter()
            .filter_map(|name| assets.get(name)),
        )
        .collect();
    let bundle = Bundle {
        static_module: &apple.static_trust,
        modules: &modules,
        installd,
    };
    Ok(switch_window::trust_bundle(&bundle)?)
}

/// The kernel window, staged into bl33.bin past the release's U-Boot, script, loader, exporter and DTB.
pub fn bl33(record: &Record, assets: &Assets, kernel: &[u8], adt: &[u8], trust: &[u8]) -> Result<Vec<u8>, Error> {
    let boot_args = std::str::from_utf8(assets.file(member::BOOT_ARGS)?)
        .map_err(|_| Error::Assets(format!("{} is not UTF-8", member::BOOT_ARGS)))?;
    let inputs = Inputs {
        kernel,
        adt,
        trust_bundle: trust,
        data_seed: assets.file(member::DATA_SEED)?,
        command_line: boot_args.trim_end_matches('\n'),
        kind: Kind {
            name: &record.build,
            virt_base: record.virt_base()?,
            root_volume_uuid: &record.os_volume_uuid,
            boot_manifest_hash: record.boot_manifest_hash,
        },
    };
    let window = switch_window::window(&switch_window::layout(&inputs)?)?;
    let boot = Boot {
        u_boot: assets.file(member::U_BOOT)?,
        script: assets.file(member::SCRIPT)?,
        loader: assets.file(member::LOADER)?,
        exporter: assets.file(member::EXPORTER)?,
        dtb: assets.file(member::DTB)?,
        loader_constants: assets.file(member::LOADER_CONSTANTS)?,
    };
    Ok(switch_window::bl33(&window, &boot)?)
}
