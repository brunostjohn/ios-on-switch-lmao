// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::{Path, PathBuf};

use serde::Serialize;

use crate::seedpack::staged_in_pack;
use crate::{PlanError, Result};

pub const SEED_REL_DIR: &str = "ngpu/addons/TegraGM20BMetal.seed/shared";
pub const PACK_REL_DIR: &str = "ngpu/addons/TegraGM20BMetal.pack";

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TreeSize {
    pub bytes: u64,
    pub files: usize,
    pub blocks4k: u64,
}

fn collect_sorted_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            collect_sorted_files(&path, out)?;
        } else if path.is_file() {
            out.push(path);
        }
    }
    out.sort();
    Ok(())
}

pub fn check_tools_match_bundle(tools_manifest: &Path, base_paths: &[PathBuf]) -> Result<String> {
    let manifest: serde_json::Value = serde_json::from_str(&fs::read_to_string(tools_manifest)?)?;
    let stack = manifest
        .get("stack")
        .or_else(|| manifest.get("stack_key"))
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| PlanError::Invalid("tools manifest missing stack key".into()))?;

    let mut info_plists = Vec::new();
    for base in base_paths {
        let mut files = Vec::new();
        if base.is_file() {
            files.push(base.clone());
        } else if base.is_dir() {
            collect_sorted_files(base, &mut files)?;
        }
        for file in files {
            if file.file_name().and_then(|n| n.to_str()) == Some("Info.plist")
                && file.parent().and_then(|p| p.file_name()).and_then(|n| n.to_str()) == Some("TegraGM20BMetal.bundle")
            {
                info_plists.push(file);
            }
        }
    }
    if info_plists.len() != 1 {
        return Err(PlanError::Invalid(
            "--base has no single TegraGM20BMetal.bundle (Info.plist)".into(),
        ));
    }
    let plist_val = plist::Value::from_file(&info_plists[0])?;
    let bundle_stack = plist_val
        .as_dictionary()
        .and_then(|d| d.get("SwitchShaderStack"))
        .and_then(plist::Value::as_string);
    if bundle_stack != Some(stack) {
        return Err(PlanError::Invalid(format!(
            "the GPU bundle's stack is {bundle_stack:?}, the shader tools' {stack}: different builds"
        )));
    }
    Ok(format!("tools {stack} match the bundle\n"))
}

pub fn native_staged_pairs(native_dir: &Path, out_dir: &Path) -> Result<Vec<(PathBuf, PathBuf)>> {
    let mut files = Vec::new();
    collect_sorted_files(native_dir, &mut files)?;
    let mut pairs = Vec::new();
    for path in files {
        let rel = path
            .strip_prefix(native_dir)
            .map_err(|e| PlanError::Invalid(e.to_string()))?;
        let mut comps = rel.components();
        let Some(first) = comps.next() else {
            continue;
        };
        let top = first.as_os_str().to_string_lossy();
        if !top.starts_with("rec-") {
            continue;
        }
        let inner = rel
            .strip_prefix(first)
            .map_err(|e| PlanError::Invalid(e.to_string()))?
            .to_string_lossy()
            .replace('\\', "/");
        if inner.is_empty() {
            continue;
        }
        if staged_in_pack(&native_dir.join(top.as_ref()), &inner)? {
            let dst = out_dir.join(PACK_REL_DIR).join("native").join(rel);
            pairs.push((path, dst));
        }
    }
    Ok(pairs)
}

pub fn layout_seed_tree(
    replies_dir: Option<&Path>,
    raw_pack_dir: Option<&Path>,
    native_dir: Option<&Path>,
    out_dir: &Path,
) -> Result<TreeSize> {
    if out_dir.exists() {
        fs::remove_dir_all(out_dir)?;
    }
    let mut pairs: Vec<(PathBuf, PathBuf)> = Vec::new();
    if let Some(seed) = replies_dir {
        let mut files = Vec::new();
        collect_sorted_files(seed, &mut files)?;
        for path in files {
            let dst = out_dir
                .join(SEED_REL_DIR)
                .join(path.strip_prefix(seed).map_err(|e| PlanError::Invalid(e.to_string()))?);
            pairs.push((path, dst));
        }
    }
    if let Some(raw) = raw_pack_dir {
        let mut files = Vec::new();
        collect_sorted_files(raw, &mut files)?;
        for path in files {
            let dst = out_dir
                .join(PACK_REL_DIR)
                .join(path.strip_prefix(raw).map_err(|e| PlanError::Invalid(e.to_string()))?);
            pairs.push((path, dst));
        }
    }
    if let Some(native) = native_dir {
        pairs.extend(native_staged_pairs(native, out_dir)?);
    }

    let mut total_bytes = 0u64;
    let mut total_blocks = 0u64;
    for (src, dst) in &pairs {
        if let Some(parent) = dst.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(src, dst)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = fs::set_permissions(dst, fs::Permissions::from_mode(0o644));
        }
        let len = fs::metadata(dst)?.len();
        total_bytes += len;
        total_blocks += len.div_ceil(4096);
    }
    Ok(TreeSize {
        bytes: total_bytes,
        files: pairs.len(),
        blocks4k: total_blocks,
    })
}
