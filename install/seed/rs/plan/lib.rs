// SPDX-License-Identifier: GPL-3.0-or-later

pub mod derive;
pub mod journal;
pub mod merge;
pub mod native_pack;
pub mod plan;
pub mod restamp;
pub mod seedpack;

pub use derive::{kernels, neighbours, targets};
pub use journal::{
    Render, Stage, compute_line, constant_key, kernel_links, ordered, parse_kernel, parse_render, parse_specialization,
    parse_stage,
};
pub use merge::{TreeSize, check_tools_match_bundle, layout_seed_tree, native_staged_pairs};
pub use native_pack::{build_coverage, native_lines, pack_lists, pack_native_seed, split_native_lines};
pub use plan::{
    DERIVED_MARKER, Depth, PlanConfig, PlanReport, bucket_lines, index_libraries, lines_for_depth, run_plan,
};
pub use restamp::{
    CACHE_ENTRY_FORMAT, CACHE_ENTRY_MAGIC, SLOT_PAGE_BYTES, cache_key_name, fnv1a64, restamp_buckets, seed_slot_name,
};
pub use seedpack::{
    DEFAULT_BUCKET_BYTES, EntrySource, IDENTITY_BYTES, PAGE_BYTES, SEED_PACK_MAGIC, SeedPackEntry, SeedPackReader,
    observed_packs, parse_seedpack_list, staged_in_pack, write_seed_pack,
};

#[derive(Debug, thiserror::Error)]
pub enum PlanError {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("crawl error: {0}")]
    Crawl(#[from] switch_seed_crawl::CrawlError),
    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("plist error: {0}")]
    Plist(#[from] plist::Error),
    #[error("{0}")]
    Invalid(String),
}

pub type Result<T> = std::result::Result<T, PlanError>;

#[cfg(test)]
mod tests;
