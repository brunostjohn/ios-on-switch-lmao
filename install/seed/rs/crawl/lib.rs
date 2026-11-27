// SPDX-License-Identifier: GPL-3.0-or-later

pub mod apfs;
pub mod crawler;
pub mod embedded;
pub mod libraries;
pub mod metallib;

pub use crawler::{
    CrawlInventory, Crawler, EmbeddedEntry, LibraryEntry, SKIP_SUFFIXES, SliceInfo, UnreadableEntry,
    format_inventory_json,
};
pub use embedded::{EmbeddedLibrary, extract_embedded_header, load_embedded_dir};
pub use libraries::{
    QC_PATH, ResolveResult, format_names_json, index_names, library_path, relocate_line, resolve_corpus,
};
pub use metallib::{
    AIR64_CPU_TYPE, AirSliceRule, Constant, EXTERN, FAT_MAGIC, FRAGMENT, Function, HEADER_LEN, KERNEL, MTLB_MAGIC,
    VERTEX, air_slice, parse_function_hashes_for_build, parse_functions, parse_functions_for_build,
    parse_functions_slice, parse_names_for_build, rule_for_build, slices, thin,
};

pub fn is_win_reserved_name(segment: &str) -> bool {
    let stem = segment.split('.').next().unwrap_or(segment);
    let upper = stem.to_ascii_uppercase();
    if matches!(upper.as_str(), "CON" | "PRN" | "AUX" | "NUL") {
        return true;
    }
    let bytes = upper.as_bytes();
    bytes.len() == 4 && (bytes.starts_with(b"COM") || bytes.starts_with(b"LPT")) && (b'1'..=b'9').contains(&bytes[3])
}

/// Validates a canonical POSIX relative path that is safe across macOS, Linux, and Windows hosts.
pub fn validate_portable_rel_path(path: &str) -> Result<()> {
    if path.is_empty() || path.starts_with('/') || path.ends_with('/') {
        return Err(CrawlError::Invalid(format!("unsafe relative path {path:?}")));
    }
    for seg in path.split('/') {
        if seg.is_empty() || seg == "." || seg == ".." || seg.ends_with('.') || seg.ends_with(' ') {
            return Err(CrawlError::Invalid(format!("unsafe path segment {seg:?} in {path:?}")));
        }
        if is_win_reserved_name(seg) {
            return Err(CrawlError::Invalid(format!(
                "reserved Windows device name {seg:?} in {path:?}"
            )));
        }
        if seg
            .bytes()
            .any(|b| b < 0x20 || b == 0x7f || matches!(b, b'\\' | b':' | b'*' | b'?' | b'"' | b'<' | b'>' | b'|'))
        {
            return Err(CrawlError::Invalid(format!("invalid host path character in {path:?}")));
        }
    }
    Ok(())
}

#[derive(Debug, thiserror::Error)]
pub enum CrawlError {
    #[error("crawl cancelled")]
    Cancelled,
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("APFS read error: {0}")]
    Apfs(#[from] switch_apfs_read::ReadError),
    #[error("decmpfs error: {0}")]
    Decmpfs(#[from] switch_decmpfs::Error),
    #[error("UDIF error: {0}")]
    Udif(#[from] udif::DppError),
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("{0}")]
    Invalid(String),
}

pub type Result<T> = std::result::Result<T, CrawlError>;

#[cfg(test)]
mod tests;
