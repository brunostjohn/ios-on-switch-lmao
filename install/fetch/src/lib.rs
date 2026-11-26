// SPDX-License-Identifier: GPL-3.0-or-later
mod error;
mod http_file;
mod range;
mod release;
mod span;
mod state;
mod sums;

pub use error::{Error, Result};
pub use http_file::HttpFile;
pub use release::{Asset, content_length, fetch_release};
pub use reqwest::Url;
pub use span::{CHECKPOINT, Span, checkpointed, fetch_span, fetch_span_with};
pub use sums::{parse_sums, verify_file};
