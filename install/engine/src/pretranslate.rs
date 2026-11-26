// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use switch_apfs_format::io::FileSource;
use switch_catalog::{Build, Role as CatalogRole};
use switch_image::parse_uuid;
use switch_translate::{CdhashPair, Inputs, Pins, Role as TranslateRole, edits};

use crate::LocalInputs;
use crate::error::{EngineError, Result};
use crate::fetch::PreparedMembers;
use crate::progress::EngineProgress;

const DEFAULT_TRANSLATOR_VERSION: &str = "switch_translate";
const CACHE_LIST_NAME: &str = "a64x-cache-pretranslated.bin";

pub struct PreparedDeltas {
    pub os_dir: PathBuf,
    pub app_cryptex_dir: PathBuf,
    pub system_cryptex_dir: PathBuf,
}

pub fn prepare_pretranslated(
    build: &Build,
    local: &LocalInputs,
    members: &PreparedMembers,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<Option<PreparedDeltas>> {
    if !build.steps.pretranslate {
        return Ok(None);
    }
    progress.check_cancel()?;

    let pins = build_pins(build);
    let cache_root = work_dir.join("cache").join(build.build()).join("pretranslated");
    fs::create_dir_all(&cache_root).map_err(|e| EngineError::io_path(&cache_root, e))?;

    let os_override = local.pretranslated_os_dir.as_deref();
    let app_override = local.pretranslated_app_cryptex_dir.as_deref();
    let sys_override = local.pretranslated_system_cryptex_dir.as_deref();

    if let (Some(os), Some(app), Some(sys)) = (os_override, app_override, sys_override) {
        progress.log("Using precomputed pretranslation delta directories");
        return Ok(Some(PreparedDeltas {
            os_dir: os.to_path_buf(),
            app_cryptex_dir: app.to_path_buf(),
            system_cryptex_dir: sys.to_path_buf(),
        }));
    }

    let decoded_trust = decode_trust_caches(build, members)?;
    let os_edits = load_edits(build, local)?;
    let translator = local
        .translator_version
        .as_deref()
        .unwrap_or(DEFAULT_TRANSLATOR_VERSION);
    let os_uuid = parse_uuid(&build.record.volumes.os_volume_uuid)?;

    let os_dir = ensure_role_delta(
        TranslateRole::Os,
        &members.os_container,
        Some(os_uuid),
        &decoded_trust.os,
        &os_edits,
        &[],
        translator,
        &pins,
        os_override,
        &cache_root,
        progress,
    )?;

    let app_cryptex_dir = ensure_role_delta(
        TranslateRole::AppCryptex,
        &members.app_cryptex,
        None,
        &decoded_trust.app,
        &BTreeMap::new(),
        &[],
        translator,
        &pins,
        app_override,
        &cache_root,
        progress,
    )?;

    let os_pairs = read_pairs(&os_dir, TranslateRole::Os)?;
    let app_pairs = read_pairs(&app_cryptex_dir, TranslateRole::AppCryptex)?;
    let mut combined_pairs = Vec::with_capacity(os_pairs.len() + app_pairs.len());
    combined_pairs.extend_from_slice(&os_pairs);
    combined_pairs.extend_from_slice(&app_pairs);

    let system_cryptex_dir = ensure_role_delta(
        TranslateRole::SystemCryptex,
        &members.system_cryptex,
        None,
        &decoded_trust.system,
        &BTreeMap::new(),
        &combined_pairs,
        translator,
        &pins,
        sys_override,
        &cache_root,
        progress,
    )?;

    Ok(Some(PreparedDeltas {
        os_dir,
        app_cryptex_dir,
        system_cryptex_dir,
    }))
}

#[allow(clippy::too_many_arguments)]
fn ensure_role_delta(
    role: TranslateRole,
    image_path: &Path,
    volume: Option<[u8; 16]>,
    apple_trust: &[Vec<u8>],
    edits_table: &BTreeMap<String, Vec<edits::Edit>>,
    pairs: &[CdhashPair],
    translator: &str,
    pins: &Pins,
    override_dir: Option<&Path>,
    cache_root: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<PathBuf> {
    progress.check_cancel()?;
    if let Some(dir) = override_dir {
        progress.log(format!("Using local {} delta {}", role.name(), dir.display()));
        return Ok(dir.to_path_buf());
    }

    let out_dir = cache_root.join(role.name());
    if delta_cache_valid(&out_dir, role, pins) {
        progress.log(format!("Reusing cached {} pretranslation delta", role.name()));
        return Ok(out_dir);
    }
    if out_dir.exists() {
        fs::remove_dir_all(&out_dir).map_err(|e| EngineError::io_path(&out_dir, e))?;
    }

    progress.log(format!("Pretranslating {} from {}", role.name(), image_path.display()));
    let src = FileSource::open(image_path, 0).map_err(|e| EngineError::io_path(image_path, e))?;
    let inputs = Inputs {
        role,
        source: &src,
        volume,
        apple_trust,
        edits: edits_table,
        pairs,
        translator,
        pins,
    };
    let delta = switch_translate::translate_file(&inputs, image_path, 0, &out_dir)?;
    for line in &delta.log {
        progress.log(line.clone());
    }
    Ok(delta.dir)
}

fn delta_cache_valid(dir: &Path, role: TranslateRole, pins: &Pins) -> bool {
    let manifest = dir.join(format!("{}.manifest.json", role.name()));
    let pairs = dir.join(format!("{}.cdhash-pairs", role.name()));
    let trust_name = format!("{}.trustcache", role.name());
    let trust_path = dir.join(&trust_name);
    if !manifest.is_file() || !pairs.is_file() || !trust_path.is_file() {
        return false;
    }
    let Ok(trust_bytes) = fs::read(&trust_path) else {
        return false;
    };
    if pins.check(&trust_name, &trust_bytes).is_err() {
        return false;
    }
    if role == TranslateRole::SystemCryptex {
        let Ok(list_bytes) = fs::read(dir.join(CACHE_LIST_NAME)) else {
            return false;
        };
        if pins.check(CACHE_LIST_NAME, &list_bytes).is_err() {
            return false;
        }
    }
    true
}

fn read_pairs(dir: &Path, role: TranslateRole) -> Result<Vec<CdhashPair>> {
    let path = dir.join(format!("{}.cdhash-pairs", role.name()));
    let text = fs::read_to_string(&path).map_err(|e| EngineError::io_path(&path, e))?;
    Ok(switch_translate::parse_pairs(&text)?)
}

fn build_pins(build: &Build) -> Pins {
    let map = build
        .record
        .derived
        .pretranslated
        .iter()
        .map(|(k, v)| (k.clone(), v.to_string()))
        .collect();
    Pins::from_map(map)
}

struct RoleTrustCaches {
    os: Vec<Vec<u8>>,
    app: Vec<Vec<u8>>,
    system: Vec<Vec<u8>>,
}

fn decode_trust_caches(build: &Build, members: &PreparedMembers) -> Result<RoleTrustCaches> {
    let im4p = members
        .im4p
        .as_ref()
        .ok_or_else(|| EngineError::Config("IM4P trust caches are required for pretranslation".into()))?;
    let static_tc = switch_img4::member_payload(&im4p.static_trust_cache, build.member(CatalogRole::StaticTrustCache))?;
    let system_tc = switch_img4::member_payload(
        &im4p.system_cryptex_trust_cache,
        build.member(CatalogRole::SystemCryptexTrustCache),
    )?;
    let app_tc = switch_img4::member_payload(
        &im4p.app_cryptex_trust_cache,
        build.member(CatalogRole::AppCryptexTrustCache),
    )?;
    Ok(RoleTrustCaches {
        os: vec![static_tc.clone()],
        app: vec![app_tc],
        system: vec![static_tc, system_tc],
    })
}

fn load_edits(build: &Build, local: &LocalInputs) -> Result<BTreeMap<String, Vec<edits::Edit>>> {
    if let Some(path) = &local.pretranslate_edits {
        let bytes = fs::read(path).map_err(|e| EngineError::io_path(path, e))?;
        return Ok(edits::parse(&bytes)?);
    }
    if let Some(dir) = &local.assets_dir {
        let candidate = dir.join(format!("edits_{}.json", build.build()));
        if candidate.is_file() {
            let bytes = fs::read(&candidate).map_err(|e| EngineError::io_path(&candidate, e))?;
            return Ok(edits::parse(&bytes)?);
        }
    }
    Ok(BTreeMap::new())
}
