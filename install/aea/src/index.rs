// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{Read, Seek, SeekFrom};

use aea_tools::crypto::{aes_aead_decrypt, derive_cluster_header_encryption_key, derive_cluster_key};
use aea_tools::prologue::AeaPrologue;
use hkdf::Hkdf;
use sha2::Sha256;

use crate::{Error, Result};

const MAGIC: &[u8; 4] = b"AEA1";
const PROFILE_SYMMETRIC: u32 = 1;
const CHECKSUM_SHA256: u8 = 2;
const COMPRESSION_LZFSE: u8 = b'e';
const COMPRESSION_NONE: u8 = b'-';
const MAC_LEN: usize = 32;
const ENTRY_LEN: usize = 8 + 32;
// The limits of Apple's AEAContext setters.
const MAX_SEGMENT_SIZE: u32 = 256 << 20;
const MAX_SEGMENTS_PER_CLUSTER: u32 = 256 << 10;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Compression {
    None,
    Lzfse,
}

#[derive(Clone, Debug)]
pub struct Segment {
    pub cluster: u32,
    pub index: u32,
    pub offset: u64,
    pub stored_len: u32,
    pub raw_offset: u64,
    pub raw_len: u32,
    pub(crate) checksum: [u8; 32],
    pub(crate) mac: [u8; 32],
}

pub(crate) struct Padding {
    pub offset: u64,
    pub len: u64,
    pub mac: [u8; 32],
}

/// An archive's authenticated layout: every segment's place, size, checksum and MAC, from the cluster headers alone.
pub struct Index {
    raw_size: u64,
    pub(crate) segments: Vec<Segment>,
    pub(crate) cluster_keys: Vec<[u8; 32]>,
    pub(crate) main_key: [u8; 32],
    pub(crate) padding: Padding,
}

struct Root {
    raw_size: u64,
    segment_size: u32,
    per_cluster: u32,
    compression: Compression,
}

impl Index {
    /// Reads the prologue and every cluster header of `archive`, which must hold the archive and nothing else.
    pub fn read<R: Read + Seek>(archive: &mut R, key: &[u8; 32]) -> Result<Index> {
        let len = archive.seek(SeekFrom::End(0)).map_err(Error::Read)?;
        let (mut prologue, fixed) = prologue(archive, len)?;
        let main_key = main_key(key, &prologue.salt, fixed);
        let root = root(&mut prologue, &main_key, len)?;
        let mut index = Index {
            raw_size: root.raw_size,
            segments: Vec::new(),
            cluster_keys: Vec::new(),
            main_key,
            padding: Padding {
                offset: 0,
                len: 0,
                mac: [0; 32],
            },
        };
        let (offset, mac) = index.walk(
            archive,
            &root,
            prologue.length() as u64,
            prologue.first_cluster_hmac,
            len,
        )?;
        index.padding = Padding {
            offset,
            len: len - offset,
            mac,
        };
        Ok(index)
    }

    pub fn raw_size(&self) -> u64 {
        self.raw_size
    }

    pub fn segments(&self) -> &[Segment] {
        &self.segments
    }

    pub fn clusters(&self) -> usize {
        self.cluster_keys.len()
    }

    // Each cluster header's MAC is in the one before it (the prologue's for cluster 0); the last one's is the
    // padding's. The archive ends once the segments hold raw_size bytes, not after container_size's worth of clusters.
    fn walk<R: Read + Seek>(
        &mut self,
        archive: &mut R,
        root: &Root,
        mut offset: u64,
        mut mac: [u8; 32],
        len: u64,
    ) -> Result<(u64, [u8; 32])> {
        let entries_len = root.per_cluster as usize * ENTRY_LEN;
        let mut header = vec![0u8; entries_len + MAC_LEN + root.per_cluster as usize * MAC_LEN];
        let mut raw_offset = 0u64;
        while raw_offset < root.raw_size {
            let cluster = self.cluster_keys.len() as u32;
            let cluster_key =
                derive_cluster_key(&self.main_key, cluster).map_err(|e| Error::Malformed(e.to_string()))?;
            if offset + header.len() as u64 > len {
                return Err(Error::Malformed(format!(
                    "cluster {cluster}'s header runs past the end"
                )));
            }
            archive.seek(SeekFrom::Start(offset)).map_err(Error::Read)?;
            archive.read_exact(&mut header).map_err(Error::Read)?;
            offset += header.len() as u64;
            let (entries, macs) = header.split_at(entries_len);
            let chek = derive_cluster_header_encryption_key(&cluster_key);
            let entries = aes_aead_decrypt(&chek, entries, macs, &mac)
                .map_err(|_| Error::Authentication(format!("cluster {cluster}'s header")))?;
            mac.copy_from_slice(&macs[..MAC_LEN]);
            let segment_macs = macs[MAC_LEN..].chunks_exact(MAC_LEN);
            for (index, (entry, segment_mac)) in entries.chunks_exact(ENTRY_LEN).zip(segment_macs).enumerate() {
                let raw_len = u32::from_le_bytes(entry[0..4].try_into().unwrap());
                let stored_len = u32::from_le_bytes(entry[4..8].try_into().unwrap());
                let expected = (root.raw_size - raw_offset).min(root.segment_size.into()) as u32;
                let malformed = |what: &str| Error::Malformed(format!("segment {cluster}/{index} {what}"));
                if raw_len != expected {
                    return Err(malformed(&format!("holds {raw_len} bytes, not {expected}")));
                }
                if expected == 0 {
                    if stored_len != 0 {
                        return Err(malformed("is past the end but stores data"));
                    }
                    continue;
                }
                if stored_len == 0 || stored_len > root.segment_size || offset + u64::from(stored_len) > len {
                    return Err(malformed(&format!("stores {stored_len} bytes")));
                }
                if root.compression == Compression::None && stored_len != raw_len {
                    return Err(malformed("is compressed in an archive without compression"));
                }
                self.segments.push(Segment {
                    cluster,
                    index: index as u32,
                    offset,
                    stored_len,
                    raw_offset,
                    raw_len,
                    checksum: entry[8..].try_into().unwrap(),
                    mac: segment_mac.try_into().unwrap(),
                });
                offset += u64::from(stored_len);
                raw_offset += u64::from(raw_len);
            }
            self.cluster_keys.push(cluster_key);
        }
        Ok((offset, mac))
    }
}

fn prologue<R: Read + Seek>(archive: &mut R, len: u64) -> Result<(AeaPrologue, [u8; 4])> {
    let mut fixed = [0u8; 12];
    archive.seek(SeekFrom::Start(0)).map_err(Error::Read)?;
    archive.read_exact(&mut fixed).map_err(Error::Read)?;
    if &fixed[..4] != MAGIC {
        return Err(Error::Unsupported("no AEA1 magic".into()));
    }
    let profile = u32::from_le_bytes([fixed[4], fixed[5], fixed[6], 0]);
    if profile != PROFILE_SYMMETRIC {
        return Err(Error::Unsupported(format!(
            "profile {profile}; only 1 (symmetric key, unsigned) is read"
        )));
    }
    if u64::from(u32::from_le_bytes(fixed[8..12].try_into().unwrap())) > len {
        return Err(Error::Malformed("its auth data runs past the end".into()));
    }
    archive.seek(SeekFrom::Start(0)).map_err(Error::Read)?;
    let prologue = AeaPrologue::decode(&mut &mut *archive).map_err(|e| Error::Malformed(format!("prologue: {e}")))?;
    Ok((prologue, fixed[4..8].try_into().unwrap()))
}

// The info string ends with the prologue's profile and scrypt hardness bytes as they are stored.
fn main_key(key: &[u8; 32], salt: &[u8; 32], profile_and_hardness: [u8; 4]) -> [u8; 32] {
    let mut info = *b"AEA_AMK\0\0\0\0";
    info[7..].copy_from_slice(&profile_and_hardness);
    let mut main_key = [0u8; 32];
    Hkdf::<Sha256>::new(Some(salt), key)
        .expand(&info, &mut main_key)
        .expect("32 bytes is a valid HKDF-SHA256 length");
    main_key
}

fn root(prologue: &mut AeaPrologue, main_key: &[u8; 32], len: u64) -> Result<Root> {
    let header = prologue
        .get_decrypted_root_header(main_key)
        .map_err(|_| Error::Authentication("the root header".into()))?;
    let container_size = u64::from_le_bytes(header.container_size);
    if container_size != len {
        return Err(Error::Malformed(format!(
            "it is {len} bytes, but its root header says {container_size}"
        )));
    }
    let checksum = header.checksum_algorithm[0];
    if checksum != CHECKSUM_SHA256 {
        return Err(Error::Unsupported(format!(
            "checksum {checksum}; only SHA-256 (2) is read"
        )));
    }
    let compression = match header.compression_algorithm[0] {
        COMPRESSION_LZFSE => Compression::Lzfse,
        COMPRESSION_NONE => Compression::None,
        other => return Err(Error::Unsupported(format!("compression {:?}", other as char))),
    };
    let root = Root {
        raw_size: u64::from_le_bytes(header.raw_size),
        segment_size: u32::from_le_bytes(header.segment_size),
        per_cluster: u32::from_le_bytes(header.segments_per_cluster),
        compression,
    };
    let cluster_size = u64::from(root.segment_size) * u64::from(root.per_cluster);
    if !(1..=MAX_SEGMENT_SIZE).contains(&root.segment_size)
        || !(1..=MAX_SEGMENTS_PER_CLUSTER).contains(&root.per_cluster)
        || root.raw_size.div_ceil(cluster_size) > u64::from(u32::MAX)
    {
        return Err(Error::Malformed("its segment size or count is out of range".into()));
    }
    Ok(root)
}
