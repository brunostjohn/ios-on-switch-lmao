// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};
use switch_catalog::{Container, Member, Role};
use switch_fetch::verify_file;

use crate::error::{EngineError, Result};
use crate::progress::EngineProgress;
use crate::resume::{load_stamp, save_stamp};

#[allow(clippy::too_many_arguments)]
pub fn prepare_local_disk_override(
    path: &Path,
    member: &Member,
    container_kind: Container,
    expected_size: u64,
    expected_sha256: &[u8; 32],
    decoded_dir: &Path,
    role: Role,
    progress: &mut EngineProgress<'_>,
) -> Result<PathBuf> {
    let mut file = File::open(path).map_err(|e| EngineError::io_path(path, e))?;
    let sniffed = switch_ipsw::sniff(&mut file).map_err(|e| EngineError::io_path(path, e))?;
    if sniffed == Some(Container::Udif) && container_kind == Container::Udif {
        let out_path = decoded_dir.join(format!("{}.apfs", role.name()));
        let stamp_path = decoded_dir.join(format!("{}.apfs.stamp.json", role.name()));
        let expected_hex = hex::encode(expected_sha256);
        if let Some(stamp) = load_stamp(&stamp_path)
            && stamp.matches_file(&out_path, expected_size, &expected_hex)
        {
            progress.log(format!("Reusing unwrapped local UDIF for {}", role.name()));
            return Ok(out_path);
        }
        progress.log(format!("Unwrapping local UDIF {} ({})", role.name(), path.display()));
        file.seek(SeekFrom::Start(0))
            .map_err(|e| EngineError::io_path(path, e))?;
        unwrap_udif_to_path(file, member, &out_path)?;
        save_stamp(&stamp_path, expected_size, &expected_hex)?;
        return Ok(out_path);
    }

    let meta = file.metadata().map_err(|e| EngineError::io_path(path, e))?;
    if meta.len() != expected_size {
        return Err(EngineError::Config(format!(
            "local {} ({}) is {} bytes, catalog pins {}",
            role.name(),
            path.display(),
            meta.len(),
            expected_size
        )));
    }
    verify_file(path, expected_sha256)?;
    progress.log(format!("Verified local {} ({})", role.name(), path.display()));
    Ok(path.to_path_buf())
}

pub fn unwrap_udif_to_path(file: File, member: &Member, final_path: &Path) -> Result<()> {
    let tmp = final_path.with_extension("apfs.tmp");
    let out = File::create(&tmp).map_err(|e| EngineError::io_path(&tmp, e))?;
    let mut writer = BufWriter::with_capacity(4 << 20, out);
    switch_ipsw::member_container(BufReader::new(file), member, &mut writer)?;
    writer.flush().map_err(|e| EngineError::io_path(&tmp, e))?;
    drop(writer);
    fs::rename(&tmp, final_path).map_err(|e| EngineError::io_path(final_path, e))
}

pub fn extract_ipsw_span(ipsw_path: &Path, member: &Member, dest: &Path) -> Result<()> {
    let mut src = File::open(ipsw_path).map_err(|e| EngineError::io_path(ipsw_path, e))?;
    src.seek(SeekFrom::Start(member.offset))
        .map_err(|e| EngineError::io_path(ipsw_path, e))?;
    let tmp = dest.with_extension("stored.tmp");
    let mut out = BufWriter::with_capacity(4 << 20, File::create(&tmp).map_err(|e| EngineError::io_path(&tmp, e))?);
    let mut hasher = Sha256::new();
    let mut remaining = member.stored_size;
    let mut buf = vec![0u8; 4 << 20];
    while remaining > 0 {
        let take = buf.len().min(remaining as usize);
        src.read_exact(&mut buf[..take])
            .map_err(|e| EngineError::io_path(ipsw_path, e))?;
        hasher.update(&buf[..take]);
        out.write_all(&buf[..take]).map_err(|e| EngineError::io_path(&tmp, e))?;
        remaining -= take as u64;
    }
    out.flush().map_err(|e| EngineError::io_path(&tmp, e))?;
    drop(out);
    let got: [u8; 32] = hasher.finalize().into();
    if got != member.sha256.0 {
        let _ = fs::remove_file(&tmp);
        return Err(EngineError::Config(format!(
            "{}: local IPSW span SHA-256 {} differs from catalog pin {}",
            member.member,
            hex::encode(got),
            member.sha256
        )));
    }
    fs::rename(&tmp, dest).map_err(|e| EngineError::io_path(dest, e))
}

pub fn decode_stored_to_file(
    stored_path: &Path,
    member: &Member,
    out_path: &Path,
    fcs_pem_cache: &mut BTreeMap<String, Vec<u8>>,
    progress: &mut EngineProgress<'_>,
) -> Result<()> {
    if member.aea.is_some() {
        let archive_file = File::open(stored_path).map_err(|e| EngineError::io_path(stored_path, e))?;
        let out_file = File::create(out_path).map_err(|e| EngineError::io_path(out_path, e))?;
        let writer = BufWriter::with_capacity(4 << 20, out_file);
        switch_aea::decrypt_member_with_key_fetch(
            archive_file,
            member,
            |url| {
                if let Some(cached) = fcs_pem_cache.get(url) {
                    return Ok(cached.clone());
                }
                progress.log(format!("Fetching Apple AEA key from {url}"));
                let pem = switch_aea::fetch_fcs_key_pem(url)?;
                fcs_pem_cache.insert(url.to_owned(), pem.clone());
                Ok(pem)
            },
            writer,
            |_done| {},
        )?;
        progress.step_progress(
            "prepare",
            member.content_size(),
            Some(member.content_size()),
            Some(format!("Decrypted {}", member.file_name())),
            true,
        );
        return Ok(());
    }
    let stored = File::open(stored_path).map_err(|e| EngineError::io_path(stored_path, e))?;
    let out = File::create(out_path).map_err(|e| EngineError::io_path(out_path, e))?;
    let mut writer = BufWriter::with_capacity(4 << 20, out);
    switch_ipsw::extract(BufReader::new(stored), member, &mut writer)?;
    writer.flush().map_err(|e| EngineError::io_path(out_path, e))
}
