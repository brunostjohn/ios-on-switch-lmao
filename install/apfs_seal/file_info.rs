// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io::{self, Cursor, Read};

use sha2::{Digest, Sha256};
use switch_apfs_btree::Record;
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs::JKey;
use switch_apfs_format::fs_records::{
    APFS_FILE_INFO_DATA_HASH, APFS_HASH_SHA256_SIZE, APFS_TYPE_FILE_INFO, FileDataHashVal, FileInfoKey,
    J_FILE_INFO_TYPE_SHIFT,
};
use zerocopy::IntoBytes;
use zerocopy::little_endian::{U16, U64};

use crate::ReplacementData;

pub const FILE_INFO_CHUNK_SIZE: usize = 16_384;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChunkHash {
    pub offset: u64,
    pub hashed_len: u16,
    pub hash: [u8; APFS_HASH_SHA256_SIZE],
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HashedReplacement {
    pub size: u64,
    pub sha256: [u8; APFS_HASH_SHA256_SIZE],
    pub chunks: Vec<ChunkHash>,
}

pub fn encode_file_info(stream_id: u64, chunk: &ChunkHash) -> Record {
    let key = FileInfoKey {
        hdr: JKey::new(stream_id, APFS_TYPE_FILE_INFO),
        info_and_lba: U64::new((u64::from(APFS_FILE_INFO_DATA_HASH) << J_FILE_INFO_TYPE_SHIFT) | chunk.offset),
    };
    let head = FileDataHashVal {
        hashed_len: U16::new(chunk.hashed_len),
        hash_size: APFS_HASH_SHA256_SIZE as u8,
    };
    let mut val = Vec::with_capacity(size_of::<FileDataHashVal>() + APFS_HASH_SHA256_SIZE);
    val.extend_from_slice(head.as_bytes());
    val.extend_from_slice(&chunk.hash);
    (key.as_bytes().to_vec(), Some(val))
}

pub fn hash_reader(reader: &mut dyn Read) -> io::Result<HashedReplacement> {
    let mut full = Sha256::new();
    let mut chunks = Vec::new();
    let mut buf = [0u8; FILE_INFO_CHUNK_SIZE];
    let mut offset = 0u64;
    loop {
        let mut filled = 0usize;
        while filled < FILE_INFO_CHUNK_SIZE {
            match reader.read(&mut buf[filled..]) {
                Ok(0) => break,
                Ok(n) => filled += n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            }
        }
        if filled == 0 {
            break;
        }
        full.update(&buf[..filled]);
        let blocks = filled.div_ceil(BLOCK_SIZE);
        let padded = blocks * BLOCK_SIZE;
        buf[filled..padded].fill(0);
        let hash: [u8; APFS_HASH_SHA256_SIZE] = Sha256::digest(&buf[..padded]).into();
        chunks.push(ChunkHash {
            offset,
            hashed_len: blocks as u16,
            hash,
        });
        offset = offset
            .checked_add(filled as u64)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "file size overflow"))?;
        if filled < FILE_INFO_CHUNK_SIZE {
            break;
        }
    }
    Ok(HashedReplacement {
        size: offset,
        sha256: full.finalize().into(),
        chunks,
    })
}

pub fn hash_data(data: &ReplacementData) -> io::Result<HashedReplacement> {
    match data {
        ReplacementData::Bytes(bytes) => hash_reader(&mut Cursor::new(bytes.as_ref())),
        ReplacementData::File(path) => {
            let mut file = File::open(path)?;
            hash_reader(&mut file)
        }
    }
}
