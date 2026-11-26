// SPDX-License-Identifier: GPL-3.0-or-later
//! Each of `//boot:package`'s steps that the installer runs, on the build's catalog record, the bundle's files and the
//! IPSW's.

use switch_adt::{Finish, TrustModules};
use switch_catalog::{Build, Prelink, Role};
use switch_prelink::{FilesetInputs, Inputs17, load_sites, prelink_17, prelink_fileset};
use switch_window::{Boot, Bundle, Inputs, Kind};

use crate::assets::{self as member, Assets};
use crate::{Error, LaunchdCatalog, Members};

/// The IPSW's IM4P payloads but the kernelcache, each checked against its catalog pins.
pub struct Apple {
    pub device_tree: Vec<u8>,
    pub static_trust: Vec<u8>,
    pub system_trust: Vec<u8>,
    pub app_trust: Vec<u8>,
}

pub fn apple(build: &Build, members: &Members) -> Result<Apple, Error> {
    let payload = |im4p: &[u8], role: Role| switch_img4::member_payload(im4p, build.member(role));
    Ok(Apple {
        device_tree: payload(members.device_tree, Role::DeviceTree)?,
        static_trust: payload(members.static_trust_cache, Role::StaticTrustCache)?,
        system_trust: payload(members.system_cryptex_trust_cache, Role::SystemCryptexTrustCache)?,
        app_trust: payload(members.app_cryptex_trust_cache, Role::AppCryptexTrustCache)?,
    })
}

/// The kernelcache's kexts prelinked into the release's kernel, the way the build's prelink step names.
pub fn prelink(build: &Build, assets: &Assets, kernelcache_im4p: &[u8]) -> Result<Vec<u8>, Error> {
    let kernelcache = switch_img4::member_payload(kernelcache_im4p, build.member(Role::Kernelcache))?;
    let kernel = assets.file(member::KERNEL)?;
    let sites: Vec<&[u8]> = assets.under(member::SITES).map(|(_, table)| table).collect();
    Ok(match build.steps.prelink {
        Prelink::Native => {
            let inputs = Inputs17::new(
                assets.file(member::PRELINK_OPTIONS)?,
                assets.file(member::PRELINK_PREVIOUS)?,
                assets.file(member::PRELINK_INHERITED)?,
                load_sites(&sites, build.build())?,
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

/// Apple's launchd catalogue with the project's jobs, for a build that runs the launchd_catalog step.
pub fn launchd(
    build: &Build,
    assets: &Assets,
    given: Option<&LaunchdCatalog>,
) -> Result<Option<switch_launchd::Catalog>, Error> {
    match (build.launchd_catalog(), given) {
        (None, None) => Ok(None),
        (Some(pins), Some(given)) => {
            let jobs = switch_launchd::Jobs::parse(
                assets.file(member::BOOTSTRAP_JOB)?,
                assets.file(member::APPS_JOB)?,
                assets.file(member::SDWAKE_JOB)?,
            )?;
            let (catalog_sha256, signature_sha256) = (pins.sha256.to_string(), pins.signature_sha256.to_string());
            let pins = switch_launchd::Pins {
                catalog_sha256: &catalog_sha256,
                signature_sha256: &signature_sha256,
            };
            Ok(Some(switch_launchd::build(given.plist, given.signature, &pins, &jobs)?))
        }
        (Some(_), None) => Err(Error::Members(
            "the build pins Apple's launchd catalogue, and none is given",
        )),
        (None, Some(_)) => Err(Error::Members(
            "a launchd catalogue is given for a build that pins none",
        )),
    }
}

/// The release's base ADT given the IPSW's iPad identity and the build's /chosen entries (adt_edit.py finish).
pub fn adt(
    build: &Build,
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
        root_volume_uuid: &build.record.volumes.os_volume_uuid,
        boot_manifest_hash: build.steps.boot_manifest_hash,
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
pub fn bl33(build: &Build, assets: &Assets, kernel: &[u8], adt: &[u8], trust: &[u8]) -> Result<Vec<u8>, Error> {
    let boot_args = std::str::from_utf8(assets.file(member::BOOT_ARGS)?)
        .map_err(|_| Error::Assets(format!("{} is not UTF-8", member::BOOT_ARGS)))?;
    let inputs = Inputs {
        kernel,
        adt,
        trust_bundle: trust,
        data_seed: assets.file(member::DATA_SEED)?,
        command_line: boot_args.trim_end_matches('\n'),
        kind: Kind {
            name: build.build(),
            virt_base: build.record.derived.kernel_virt_base,
            root_volume_uuid: &build.record.volumes.os_volume_uuid,
            boot_manifest_hash: build.steps.boot_manifest_hash,
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
