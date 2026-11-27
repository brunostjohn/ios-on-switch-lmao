// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::BufReader;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use reqwest::{Client, Url};
use switch_catalog::{Asset, Build, Catalog, Container, Member, Role};
use switch_fetch::{Span, fetch_release, fetch_span, verify_file};

use crate::LocalInputs;
use crate::decode::{decode_stored_to_file, extract_ipsw_span, prepare_local_disk_override, unwrap_udif_to_path};
use crate::error::{EngineError, Result};
use crate::progress::{CancelToken, EngineProgress};
use crate::resume::{load_stamp, save_stamp};

pub const HTTP_CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
pub const HTTP_READ_TIMEOUT: Duration = Duration::from_secs(60);

pub fn build_http_client() -> Result<Client> {
    Client::builder()
        .connect_timeout(HTTP_CONNECT_TIMEOUT)
        .read_timeout(HTTP_READ_TIMEOUT)
        .user_agent("switch-ios-installer")
        .build()
        .map_err(|e| EngineError::Http(e.to_string()))
}

pub fn load_catalog(local_path: Option<&Path>, url_override: Option<&str>, tag: Option<&str>) -> Result<Catalog> {
    if let Some(path) = local_path {
        let bytes = fs::read(path).map_err(|e| EngineError::io_path(path, e))?;
        return Ok(Catalog::parse(&bytes)?);
    }
    let url_str = match url_override {
        Some(u) => u.to_owned(),
        None => switch_catalog::catalog_url(tag),
    };
    let client = build_http_client()?;
    let rt = new_runtime()?;
    let bytes = rt.block_on(async move {
        let resp = client
            .get(&url_str)
            .send()
            .await
            .and_then(|r| r.error_for_status())
            .map_err(|e| EngineError::Http(format!("{url_str}: {e}")))?;
        resp.bytes()
            .await
            .map(|b| b.to_vec())
            .map_err(|e| EngineError::Http(format!("{url_str}: {e}")))
    })?;
    Ok(Catalog::parse(&bytes)?)
}

pub struct Im4pMembers {
    pub kernelcache: Vec<u8>,
    pub device_tree: Vec<u8>,
    pub static_trust_cache: Vec<u8>,
    pub system_cryptex_trust_cache: Vec<u8>,
    pub app_cryptex_trust_cache: Vec<u8>,
}

pub struct PreparedMembers {
    pub os_container: PathBuf,
    pub system_cryptex: PathBuf,
    pub app_cryptex: PathBuf,
    pub im4p: Option<Im4pMembers>,
}

#[derive(Clone, Debug, Default)]
pub struct PreparedAssets {
    pub boot_tar: Option<PathBuf>,
    pub overlay_tar: Option<PathBuf>,
    pub bootstrap_tar: Option<PathBuf>,
    pub gpu_tar: Option<PathBuf>,
    pub seed_tar: Option<PathBuf>,
}

pub fn resolve_release_assets(
    catalog: &Catalog,
    build: &Build,
    local: &LocalInputs,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<PreparedAssets> {
    let boot_tar = local
        .boot_files_dir
        .is_none()
        .then(|| resolve_boot_asset_tar(catalog, build, local, work_dir, progress))
        .transpose()?;
    let assets_dir = local.assets_dir.as_deref();
    let mut optional = |role: &str, override_set: bool| -> Result<Option<PathBuf>> {
        if override_set {
            Ok(None)
        } else {
            resolve_release_asset(catalog, build, role, None, assets_dir, work_dir, progress)
        }
    };
    Ok(PreparedAssets {
        boot_tar,
        overlay_tar: optional("overlay", local.overlay_dir.is_some())?,
        bootstrap_tar: optional("bootstrap", local.bootstrap_dir.is_some())?,
        gpu_tar: optional("gpu", local.gpu_dir.is_some())?,
        seed_tar: optional("seed", local.shader_seed_dir.is_some())?,
    })
}

pub fn resolve_boot_asset_tar(
    catalog: &Catalog,
    build: &Build,
    local: &LocalInputs,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<PathBuf> {
    let _ = build.asset("boot")?;
    resolve_release_asset(
        catalog,
        build,
        "boot",
        local.boot_tar.as_deref(),
        local.assets_dir.as_deref(),
        work_dir,
        progress,
    )?
    .ok_or_else(|| {
        EngineError::Catalog(switch_catalog::Error::NoAsset {
            build: build.build().to_owned(),
            role: "boot".to_owned(),
        })
    })
}

pub fn resolve_release_asset(
    catalog: &Catalog,
    build: &Build,
    role: &str,
    override_path: Option<&Path>,
    assets_dir: Option<&Path>,
    work_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<Option<PathBuf>> {
    progress.check_cancel()?;
    if let Some(path) = override_path {
        progress.log(format!("Using local {role} asset {}", path.display()));
        return Ok(Some(path.to_path_buf()));
    }
    let Some(asset) = build.record.assets.get(role) else {
        return Ok(None);
    };
    if let Some(candidate) = assets_dir.map(|d| d.join(&asset.name)).filter(|p| p.is_file()) {
        progress.log(format!("Using local {role} asset {}", candidate.display()));
        return Ok(Some(candidate));
    }

    let assets_cache = work_dir.join("cache").join(&catalog.release).join("assets");
    fs::create_dir_all(&assets_cache).map_err(|e| EngineError::io_path(&assets_cache, e))?;
    let dest = assets_cache.join(&asset.name);
    let stamp_path = assets_cache.join(format!("{}.stamp.json", asset.name));
    let expected_hex = asset.sha256.to_string();
    if dest.is_file() {
        if load_stamp(&stamp_path).is_some_and(|s| s.matches_file(&dest, asset.size, &expected_hex)) {
            progress.log(format!("Reusing cached {role} asset {}", dest.display()));
            return Ok(Some(dest));
        }
        if check_asset_matches(asset, &dest).is_ok() {
            save_stamp(&stamp_path, asset.size, &expected_hex)?;
            progress.log(format!("Reusing cached {role} asset {}", dest.display()));
            return Ok(Some(dest));
        }
        let _ = fs::remove_file(&dest);
    }

    let base_url = Url::parse(&catalog.assets_url())
        .map_err(|e| EngineError::Http(format!("invalid release URL {}: {e}", catalog.assets_url())))?;
    let client = build_http_client()?;
    let cancel = progress.cancel_token();
    let total = asset.size;
    let progress_bytes = Arc::new(Mutex::new(0u64));
    let progress_cb = Arc::clone(&progress_bytes);
    progress.log(format!(
        "Downloading {role} release asset {} ({total} bytes)",
        asset.name
    ));
    run_cancelable(&cancel, async move {
        fetch_release(
            &client,
            &base_url,
            std::slice::from_ref(asset),
            &assets_cache,
            move |_, done| {
                if let Ok(mut g) = progress_cb.lock() {
                    *g = done;
                }
            },
        )
        .await
        .map_err(EngineError::Fetch)
    })?;
    save_stamp(&stamp_path, asset.size, &expected_hex)?;
    let done = progress_bytes.lock().map_or(total, |g| *g);
    progress.step_progress(
        "prepare",
        done.max(total),
        Some(total),
        Some(format!("Fetched {}", asset.name)),
        true,
    );
    Ok(Some(dest))
}

pub fn prepare_members(
    build: &Build,
    local: &LocalInputs,
    work_dir: &Path,
    need_im4p: bool,
    progress: &mut EngineProgress<'_>,
) -> Result<PreparedMembers> {
    let build_cache = work_dir.join("cache").join(build.build());
    let ipsw_dir = build_cache.join("ipsw");
    let decoded_dir = build_cache.join("decoded");
    fs::create_dir_all(&ipsw_dir).map_err(|e| EngineError::io_path(&ipsw_dir, e))?;
    fs::create_dir_all(&decoded_dir).map_err(|e| EngineError::io_path(&decoded_dir, e))?;

    let mut fcs_pem_cache = BTreeMap::new();
    let os_container = prepare_disk_member(
        build,
        Role::Os,
        local.os_dmg.as_deref(),
        local.ipsw_path.as_deref(),
        &ipsw_dir,
        &decoded_dir,
        &mut fcs_pem_cache,
        progress,
    )?;
    let system_cryptex = prepare_disk_member(
        build,
        Role::SystemCryptex,
        local.system_cryptex.as_deref(),
        local.ipsw_path.as_deref(),
        &ipsw_dir,
        &decoded_dir,
        &mut fcs_pem_cache,
        progress,
    )?;
    let app_cryptex = prepare_disk_member(
        build,
        Role::AppCryptex,
        local.app_cryptex.as_deref(),
        local.ipsw_path.as_deref(),
        &ipsw_dir,
        &decoded_dir,
        &mut fcs_pem_cache,
        progress,
    )?;

    let im4p = if need_im4p {
        Some(Im4pMembers {
            kernelcache: prepare_im4p_member(
                build,
                Role::Kernelcache,
                local.ipsw_path.as_deref(),
                &ipsw_dir,
                &decoded_dir,
                progress,
            )?,
            device_tree: prepare_im4p_member(
                build,
                Role::DeviceTree,
                local.ipsw_path.as_deref(),
                &ipsw_dir,
                &decoded_dir,
                progress,
            )?,
            static_trust_cache: prepare_im4p_member(
                build,
                Role::StaticTrustCache,
                local.ipsw_path.as_deref(),
                &ipsw_dir,
                &decoded_dir,
                progress,
            )?,
            system_cryptex_trust_cache: prepare_im4p_member(
                build,
                Role::SystemCryptexTrustCache,
                local.ipsw_path.as_deref(),
                &ipsw_dir,
                &decoded_dir,
                progress,
            )?,
            app_cryptex_trust_cache: prepare_im4p_member(
                build,
                Role::AppCryptexTrustCache,
                local.ipsw_path.as_deref(),
                &ipsw_dir,
                &decoded_dir,
                progress,
            )?,
        })
    } else {
        None
    };

    Ok(PreparedMembers {
        os_container,
        system_cryptex,
        app_cryptex,
        im4p,
    })
}

#[allow(clippy::too_many_arguments)]
fn prepare_disk_member(
    build: &Build,
    role: Role,
    local_override: Option<&Path>,
    local_ipsw: Option<&Path>,
    ipsw_dir: &Path,
    decoded_dir: &Path,
    fcs_pem_cache: &mut BTreeMap<String, Vec<u8>>,
    progress: &mut EngineProgress<'_>,
) -> Result<PathBuf> {
    progress.check_cancel()?;
    let member = build.member(role);
    let (container_kind, container_pins) = build
        .container(role)
        .ok_or_else(|| EngineError::Config(format!("{} has no container pins", role.name())))?;
    let expected_hex = container_pins.sha256.to_string();

    if let Some(path) = local_override {
        return prepare_local_disk_override(
            path,
            member,
            container_kind,
            container_pins.size,
            &container_pins.sha256.0,
            decoded_dir,
            role,
            progress,
        );
    }

    let final_path = decoded_dir.join(format!("{}.apfs", role.name()));
    let stamp_path = decoded_dir.join(format!("{}.apfs.stamp.json", role.name()));
    if let Some(stamp) = load_stamp(&stamp_path)
        && stamp.matches_file(&final_path, container_pins.size, &expected_hex)
    {
        progress.log(format!("Reusing cached {} container", role.name()));
        return Ok(final_path);
    }

    let stored_path = acquire_stored_member(build, role, member, local_ipsw, ipsw_dir, progress)?;
    progress.check_cancel()?;

    let content_path = decoded_dir.join(format!("{}.content", role.name()));
    let content_stamp = decoded_dir.join(format!("{}.content.stamp.json", role.name()));
    let content_hex = member.content_sha256.to_string();
    let content_ready =
        load_stamp(&content_stamp).is_some_and(|s| s.matches_file(&content_path, member.content_size(), &content_hex));

    if !content_ready {
        progress.log(format!("Decoding {} ({})", role.name(), member.file_name()));
        let tmp = decoded_dir.join(format!("{}.content.tmp", role.name()));
        decode_stored_to_file(&stored_path, member, &tmp, fcs_pem_cache, progress)?;
        fs::rename(&tmp, &content_path).map_err(|e| EngineError::io_path(&content_path, e))?;
        save_stamp(&content_stamp, member.content_size(), &content_hex)?;
    }
    progress.check_cancel()?;

    match container_kind {
        Container::Apfs => {
            if container_pins.size != member.content_size() || container_pins.sha256 != member.content_sha256 {
                verify_file(&content_path, &container_pins.sha256.0)?;
            }
            if final_path != content_path {
                fs::rename(&content_path, &final_path).map_err(|e| EngineError::io_path(&final_path, e))?;
            }
            save_stamp(&stamp_path, container_pins.size, &expected_hex)?;
            Ok(final_path)
        }
        Container::Udif => {
            progress.log(format!("Unwrapping UDIF {} container", role.name()));
            let src = File::open(&content_path).map_err(|e| EngineError::io_path(&content_path, e))?;
            unwrap_udif_to_path(src, member, &final_path)?;
            save_stamp(&stamp_path, container_pins.size, &expected_hex)?;
            Ok(final_path)
        }
    }
}

fn prepare_im4p_member(
    build: &Build,
    role: Role,
    local_ipsw: Option<&Path>,
    ipsw_dir: &Path,
    decoded_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<Vec<u8>> {
    progress.check_cancel()?;
    let member = build.member(role);
    let cache_path = decoded_dir.join(format!("{}.im4p", role.name()));
    let stamp_path = decoded_dir.join(format!("{}.im4p.stamp.json", role.name()));
    let content_hex = member.content_sha256.to_string();
    if let Some(stamp) = load_stamp(&stamp_path)
        && stamp.matches_file(&cache_path, member.size, &content_hex)
    {
        return fs::read(&cache_path).map_err(|e| EngineError::io_path(&cache_path, e));
    }

    let stored_path = acquire_stored_member(build, role, member, local_ipsw, ipsw_dir, progress)?;
    progress.check_cancel()?;
    let stored = File::open(&stored_path).map_err(|e| EngineError::io_path(&stored_path, e))?;
    let mut out = Vec::with_capacity(member.size as usize);
    switch_ipsw::extract(BufReader::new(stored), member, &mut out)?;
    fs::write(&cache_path, &out).map_err(|e| EngineError::io_path(&cache_path, e))?;
    save_stamp(&stamp_path, member.size, &content_hex)?;
    Ok(out)
}

fn acquire_stored_member(
    build: &Build,
    role: Role,
    member: &Member,
    local_ipsw: Option<&Path>,
    ipsw_dir: &Path,
    progress: &mut EngineProgress<'_>,
) -> Result<PathBuf> {
    let dest = ipsw_dir.join(format!("{}.stored", role.name()));
    if dest.is_file() && fs::metadata(&dest).is_ok_and(|m| m.len() == member.stored_size) {
        return Ok(dest);
    }

    if let Some(ipsw_path) = local_ipsw {
        progress.log(format!(
            "Reading {} from local IPSW {}",
            role.name(),
            ipsw_path.display()
        ));
        extract_ipsw_span(ipsw_path, member, &dest)?;
        return Ok(dest);
    }

    let url = Url::parse(&build.record.ipsw.url)
        .map_err(|e| EngineError::Http(format!("invalid IPSW URL {}: {e}", build.record.ipsw.url)))?;
    let span = Span::member(&url, member);
    let client = build_http_client()?;
    let cancel = progress.cancel_token();
    let total = span.len;
    let role_name = role.name().to_owned();
    progress.log(format!("Downloading IPSW member {} ({total} bytes)", role.name()));

    let dest_clone = dest.clone();
    run_cancelable(&cancel, async move {
        fetch_span(&client, &span, &dest_clone, |_done| {})
            .await
            .map_err(EngineError::Fetch)
    })?;
    progress.step_progress(
        "prepare",
        total,
        Some(total),
        Some(format!("Fetched {role_name}")),
        true,
    );
    Ok(dest)
}

fn new_runtime() -> Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .map_err(|e| EngineError::io("tokio runtime", e))
}

fn run_cancelable<F, T>(cancel: &CancelToken, fut: F) -> Result<T>
where
    F: std::future::Future<Output = Result<T>>,
{
    if cancel.is_cancelled() {
        return Err(EngineError::Cancelled);
    }
    let rt = new_runtime()?;
    let cancel_poll = cancel.clone();
    rt.block_on(async move {
        tokio::select! {
            res = fut => res,
            () = async move {
                loop {
                    if cancel_poll.is_cancelled() {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(75)).await;
                }
            } => Err(EngineError::Cancelled),
        }
    })
}

pub fn check_asset_matches(asset: &Asset, path: &Path) -> Result<()> {
    let file = File::open(path).map_err(|e| EngineError::io_path(path, e))?;
    asset.check(BufReader::new(file))?;
    Ok(())
}
