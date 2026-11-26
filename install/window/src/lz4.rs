// SPDX-License-Identifier: GPL-3.0-or-later
//! Each block of the window compressed on its own, byte for byte as `lz4 -12 -B6 --no-frame-crc` compresses it
//! (lz4 1.10.0, the C //tools:lz4 builds), and its body taken from the frame as build_native_loader.py
//! lz4_frame_blocks takes it.

use std::ptr::null;

use lz4_sys::{
    BlockChecksum, BlockMode, BlockSize, ContentChecksum, FrameType, LZ4F_VERSION, LZ4F_compressBegin,
    LZ4F_compressBound, LZ4F_compressEnd, LZ4F_compressUpdate, LZ4F_createCompressionContext,
    LZ4F_freeCompressionContext, LZ4F_getErrorName, LZ4F_isError, LZ4FCompressionContext, LZ4FFrameInfo,
    LZ4FPreferences, size_t,
};
use rayon::prelude::*;
use switch_loader_abi::LZ4_MAGIC;

use crate::Error;

const LEVEL: u32 = 12;
const HEADER_SIZE_MAX: usize = 19;

/// One block as the loader's table stores it: stored blocks are the window's bytes as they are.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Block {
    pub stored: bool,
    pub body: Vec<u8>,
}

struct Context(LZ4FCompressionContext);

impl Context {
    fn new() -> Result<Self, Error> {
        let mut ctx = LZ4FCompressionContext(std::ptr::null_mut());
        checked(unsafe { LZ4F_createCompressionContext(&mut ctx, LZ4F_VERSION) })?;
        Ok(Self(ctx))
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        unsafe { LZ4F_freeCompressionContext(self.0) };
    }
}

fn checked(code: size_t) -> Result<usize, Error> {
    if unsafe { LZ4F_isError(code) } == 0 {
        return Ok(code);
    }
    let name = unsafe { std::ffi::CStr::from_ptr(LZ4F_getErrorName(code)) };
    Err(Error::Lz4(name.to_string_lossy().into_owned()))
}

fn preferences() -> LZ4FPreferences {
    LZ4FPreferences {
        frame_info: LZ4FFrameInfo {
            block_size_id: BlockSize::Max1MB,
            block_mode: BlockMode::Independent,
            content_checksum_flag: ContentChecksum::NoChecksum,
            frame_type: FrameType::Frame,
            content_size: 0,
            dict_id: 0,
            block_checksum_flag: BlockChecksum::NoBlockChecksum,
        },
        compression_level: LEVEL,
        auto_flush: 1,
        favor_dec_speed: 0,
        reserved: [0; 3],
    }
}

fn frame(ctx: &Context, data: &[u8]) -> Result<Vec<u8>, Error> {
    let prefs = preferences();
    let mut out = vec![0u8; HEADER_SIZE_MAX + unsafe { LZ4F_compressBound(data.len(), &prefs) }];
    let mut at = checked(unsafe { LZ4F_compressBegin(ctx.0, out.as_mut_ptr(), out.len(), &prefs) })?;
    at += checked(unsafe {
        LZ4F_compressUpdate(
            ctx.0,
            out[at..].as_mut_ptr(),
            out.len() - at,
            data.as_ptr(),
            data.len(),
            null(),
        )
    })?;
    at += checked(unsafe { LZ4F_compressEnd(ctx.0, out[at..].as_mut_ptr(), out.len() - at, null()) })?;
    out.truncate(at);
    Ok(out)
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

/// An LZ4 frame of `length` bytes split into its blocks as (decoded length, block): only what the loader decodes,
/// independent blocks without checksums or a dictionary.
pub fn frame_blocks(frame: &[u8], length: usize) -> Result<Vec<(usize, Block)>, Error> {
    if frame.len() < 7 || frame[..4] != LZ4_MAGIC {
        return Err(Error::Lz4("not an LZ4 frame".into()));
    }
    let (flags, descriptor) = (frame[4], frame[5]);
    if flags >> 6 != 1 || flags >> 5 & 1 == 0 || flags >> 4 & 1 != 0 || flags & 1 != 0 || (descriptor >> 4 & 7) < 4 {
        return Err(Error::Lz4(format!(
            "unsupported frame flags {flags:#x}/{descriptor:#x}"
        )));
    }
    let block_max = 1usize << (8 + 2 * (descriptor >> 4 & 7));
    let (mut at, mut left, mut blocks) = (6 + if flags >> 3 & 1 != 0 { 8 } else { 0 } + 1, length, Vec::new());
    loop {
        let size = u32_at(frame, at).ok_or_else(|| Error::Lz4("truncated frame".into()))?;
        at += 4;
        if size == 0 {
            break;
        }
        let (stored, size) = (size >> 31 != 0, (size & 0x7FFF_FFFF) as usize);
        if size == 0 || left == 0 || at + size > frame.len() {
            return Err(Error::Lz4("truncated frame".into()));
        }
        let decoded = block_max.min(left);
        blocks.push((
            decoded,
            Block {
                stored,
                body: frame[at..at + size].to_vec(),
            },
        ));
        left -= decoded;
        at += size;
    }
    if left != 0 || at + if flags >> 2 & 1 != 0 { 4 } else { 0 } != frame.len() {
        return Err(Error::Lz4("frame does not hold exactly the extent".into()));
    }
    Ok(blocks)
}

fn compress_one(ctx: &Context, data: &[u8]) -> Result<Block, Error> {
    let mut blocks = frame_blocks(&frame(ctx, data)?, data.len())?;
    match blocks.pop() {
        Some((decoded, block)) if blocks.is_empty() && decoded == data.len() => Ok(block),
        _ => Err(Error::Lz4("a block did not compress to one block".into())),
    }
}

/// Every block compressed, in parallel, each with nothing carried over from another.
pub fn compress_blocks(blocks: &[&[u8]]) -> Result<Vec<Block>, Error> {
    blocks
        .par_iter()
        .map_init(Context::new, |ctx, data| {
            compress_one(ctx.as_ref().map_err(|e| Error::Lz4(e.to_string()))?, data)
        })
        .collect()
}
