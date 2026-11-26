// SPDX-License-Identifier: GPL-3.0-or-later
//! The window as the loader receives it (build_native_loader.py pack_window, lz4_blocks, block_table and
//! staged_window): only its nonzero extents, cut into 1 MiB blocks from each extent's start, the blocks back to back
//! and padded to a page, named by the SHA-256 of what is staged.

use sha2::{Digest, Sha256};
use switch_loader_abi::{LZ4_BLOCK, PACK_MIN_GAP, PACK_PAGE, STAGE_END, STAGE_SOURCE};

use crate::Error;
use crate::checks::require;
use crate::constants::{Constants, Row, STORED};
use crate::lz4::Block;

const PAGE: usize = PACK_PAGE as usize;

fn zero(page: &[u8]) -> bool {
    page.iter().all(|b| *b == 0)
}

/// The image's nonzero extents as (offset, bytes), 4 KiB granular, zero gaps shorter than PACK_MIN_GAP kept inside.
pub fn extents(image: &[u8]) -> Vec<(usize, usize)> {
    let mut runs: Vec<(usize, usize)> = Vec::new();
    let mut pages = image.chunks(PAGE).enumerate().peekable();
    while let Some((index, page)) = pages.next() {
        if zero(page) {
            continue;
        }
        let start = index * PAGE;
        let mut end = start + page.len();
        while let Some((_, page)) = pages.next_if(|(_, page)| !zero(page)) {
            end += page.len();
        }
        match runs.last_mut() {
            Some((at, size)) if start - (*at + *size) < PACK_MIN_GAP as usize => *size = end - *at,
            _ => runs.push((start, end - start)),
        }
    }
    runs
}

/// Each extent's LZ4 blocks as (window offset, length).
pub fn blocks(runs: &[(usize, usize)]) -> Vec<(usize, usize)> {
    let block = LZ4_BLOCK as usize;
    runs.iter()
        .flat_map(|&(start, size)| {
            (start..start + size)
                .step_by(block)
                .map(move |at| (at, block.min(start + size - at)))
        })
        .collect()
}

pub fn stage_id(staged: &[u8]) -> u64 {
    u64::from_le_bytes(Sha256::digest(staged)[..8].try_into().expect("SHA-256 is 32 bytes"))
}

/// The staged window: the blocks back to back, padded to a page, and the constants with its table, sizes and id.
pub fn staged(
    constants: &Constants,
    blocks: &[(usize, usize)],
    compressed: &[Block],
) -> Result<(Constants, Vec<u8>), Error> {
    let mut table: Vec<Row> = Vec::with_capacity(blocks.len());
    let mut stream = Vec::with_capacity(compressed.iter().map(|b| b.body.len()).sum::<usize>() + PAGE);
    for (&(at, length), block) in blocks.iter().zip(compressed) {
        require((at | length) % PAGE == 0, "lz4: block not page-granular")?;
        let bytes = block.body.len() as u64 | if block.stored { STORED } else { 0 };
        table.push([at as u64, length as u64, stream.len() as u64, bytes]);
        stream.extend_from_slice(&block.body);
    }
    let stream_bytes = stream.len();
    stream.resize(stream_bytes.next_multiple_of(PAGE), 0);
    require(
        STAGE_SOURCE + stream.len() as u64 + PACK_PAGE <= STAGE_END,
        "compressed kernel window does not fit before ramoops",
    )?;
    let mut constants = constants.clone();
    constants.set("PACKED_BYTES", stream.len() as u64);
    constants.set("STREAM_BYTES", stream_bytes as u64);
    constants.set("STAGE_ID", stage_id(&stream));
    constants.blocks = table;
    Ok((constants, stream))
}
