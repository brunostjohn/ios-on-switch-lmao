// SPDX-License-Identifier: GPL-3.0-or-later
use std::cell::RefCell;
use std::io::{Read, Seek, SeekFrom};

use aea_tools::crypto::{derive_padding_authentication_key, derive_segment_key};
use aes::Aes256;
use ctr::cipher::{KeyIvInit, StreamCipher};
use hmac::{Hmac, KeyInit, Mac};
use lzfse_rust::LzfseDecoder;
use sha2::{Digest, Sha256};

use crate::index::{Index, Segment};
use crate::{Error, Result};

type HmacSha256 = Hmac<Sha256>;
type Aes256Ctr = ctr::Ctr128BE<Aes256>;

thread_local! {
    static LZFSE: RefCell<LzfseDecoder> = RefCell::new(LzfseDecoder::default());
}

impl Index {
    /// One segment's bytes, from its stored bytes alone (`stored_len` of them at `offset` in the archive).
    pub fn decode(&self, segment: &Segment, stored: Vec<u8>) -> Result<Vec<u8>> {
        let plain = self.open(segment, stored)?;
        // A segment that did not compress is stored as it is; any other is an LZFSE stream of any block kind.
        let raw = if segment.stored_len == segment.raw_len {
            plain
        } else {
            let mut raw = Vec::with_capacity(segment.raw_len as usize);
            LZFSE
                .with_borrow_mut(|decoder| decoder.decode_bytes(&plain, &mut raw))
                .map_err(|e| failure(segment, format!("LZFSE: {e}")))?;
            if raw.len() != segment.raw_len as usize {
                return Err(failure(
                    segment,
                    format!("decodes to {} bytes, not {}", raw.len(), segment.raw_len),
                ));
            }
            raw
        };
        if Sha256::digest(&raw).as_slice() != segment.checksum.as_slice() {
            return Err(failure(segment, "its SHA-256 is not its checksum".into()));
        }
        Ok(raw)
    }

    pub(crate) fn open(&self, segment: &Segment, mut stored: Vec<u8>) -> Result<Vec<u8>> {
        if stored.len() != segment.stored_len as usize {
            return Err(failure(
                segment,
                format!("{} stored bytes, not {}", stored.len(), segment.stored_len),
            ));
        }
        let key = derive_segment_key(&self.cluster_keys[segment.cluster as usize], segment.index);
        HmacSha256::new_from_slice(&key[..32])
            .expect("HMAC takes any key length")
            .chain_update(&stored)
            .chain_update(0u64.to_le_bytes())
            .verify_slice(&segment.mac)
            .map_err(|_| {
                failure(
                    segment,
                    "its HMAC does not match: a wrong key, or a damaged archive".into(),
                )
            })?;
        Aes256Ctr::new_from_slices(&key[32..64], &key[64..80])
            .expect("the segment key holds an AES-256 key and IV")
            .apply_keystream(&mut stored);
        Ok(stored)
    }

    // An archive written without padding carries no MAC of its (empty) padding: aea leaves another value there.
    pub(crate) fn verify_padding<R: Read + Seek>(&self, archive: &mut R) -> Result<()> {
        if self.padding.len == 0 {
            return Ok(());
        }
        let mut mac = HmacSha256::new_from_slice(&derive_padding_authentication_key(&self.main_key))
            .expect("HMAC takes any key length");
        archive
            .seek(SeekFrom::Start(self.padding.offset))
            .map_err(Error::Read)?;
        let mut buffer = vec![0u8; 1 << 16];
        let mut left = self.padding.len;
        while left > 0 {
            let chunk = &mut buffer[..left.min(1 << 16) as usize];
            archive.read_exact(chunk).map_err(Error::Read)?;
            mac.update(chunk);
            left -= chunk.len() as u64;
        }
        mac.verify_slice(&self.padding.mac)
            .map_err(|_| Error::Authentication("the padding".into()))
    }
}

fn failure(segment: &Segment, reason: String) -> Error {
    Error::Segment {
        cluster: segment.cluster,
        index: segment.index,
        reason,
    }
}
