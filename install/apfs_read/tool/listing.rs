// SPDX-License-Identifier: GPL-3.0-or-later
//! The listing `mount_list.py` prints for the same volume through macOS's kernel:
//! `path kind ino uid:gid mode size flags nlink mtime xattrs content`, tab-separated.
//!
//! A compressed file's content is its stored bytes, `c<type>:<sha256 of decmpfs>:<sha256 of the resource fork>`,
//! since the kernel would decompress it; an uncompressed file's is the SHA-256 of its data, a symlink's its target.

use sha2::{Digest, Sha256};
use switch_apfs_format::BLOCK_SIZE;
use switch_apfs_format::fs_records::{
    APFS_FILE_INFO_DATA_HASH, DECMPFS_XATTR_NAME, RESOURCE_FORK_XATTR_NAME, SYMLINK_XATTR_NAME,
};
use switch_apfs_format::io::BlockSource;
use switch_apfs_read::{Entry, ReadError, Record, Stream, Xattr, XattrData, stream};

pub fn uuid(b: &[u8; 16]) -> String {
    let h = hex::encode_upper(b);
    format!("{}-{}-{}-{}-{}", &h[..8], &h[8..12], &h[12..16], &h[16..20], &h[20..])
}

pub fn kind_name(r: &Record) -> &'static str {
    match r {
        Record::Inode(_) => "inode",
        Record::DirRec(_) => "drec",
        Record::Xattr(_) => "xattr",
        Record::SiblingLink(_) => "sibling",
        Record::SiblingMap { .. } => "sibling-map",
        Record::DstreamId { .. } => "dstream-id",
        Record::FileExtent(_) => "file-extent",
        Record::FileInfo(_) => "file-info",
        Record::DirStats { .. } => "dir-stats",
        Record::Other { .. } => "other",
    }
}

pub fn escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\t', "\\t").replace('\n', "\\n")
}

/// Whether to hash this path's content: one path in `every`, by the first byte of the path's SHA-256.
pub fn sampled(path: &str, every: u32) -> bool {
    every != 0 && u32::from(Sha256::digest(path.as_bytes())[0]) % every == 0
}

fn bytes(src: &dyn BlockSource, x: &XattrData, s: Option<&Stream>) -> Result<Vec<u8>, ReadError> {
    match (x, s) {
        (XattrData::Embedded(b), _) => Ok(b.clone()),
        (XattrData::Stream { .. }, Some(s)) => {
            let mut out = Vec::new();
            stream::read(src, s, &mut |c| out.extend_from_slice(c))?;
            Ok(out)
        }
        (XattrData::Stream { id, .. }, None) => Err(ReadError::invalid(format!("stream {id} unread"))),
    }
}

fn xattr_len(x: &Xattr, s: Option<&Stream>) -> u64 {
    match (&x.data, s) {
        (XattrData::Embedded(b), _) => b.len() as u64,
        (XattrData::Stream { .. }, Some(s)) => s.size,
        (XattrData::Stream { dstream, .. }, None) => dstream.dstream.size.get(),
    }
}

fn sha(src: &dyn BlockSource, s: &Stream) -> Result<String, ReadError> {
    let mut h = Sha256::new();
    stream::read(src, s, &mut |c| h.update(c))?;
    Ok(hex::encode(h.finalize()))
}

/// `expanded`: the volume has `APFS_INCOMPAT_EXPANDED_RECORDS`, under which macOS counts a directory's links from its
/// subdirectories (`uncompressed_size`) rather than from all its children.
pub fn line(src: &dyn BlockSource, e: &Entry, hash_every: u32, expanded: bool) -> Result<String, ReadError> {
    let v = &e.inode.val;
    let compression = stream::compression(src, e)?;
    let symlink = match e.xattr(SYMLINK_XATTR_NAME) {
        Some((x, s)) if e.inode.is_symlink() => {
            let mut target = bytes(src, &x.data, s.as_ref())?;
            if target.pop() != Some(0) {
                return Err(ReadError::invalid(format!(
                    "{}: symlink target isn't NUL-terminated",
                    e.path
                )));
            }
            Some(String::from_utf8_lossy(&target).into_owned())
        }
        _ => None,
    };
    let (kind, size, nlink, content) = if e.inode.is_dir() {
        // macOS's APFS reports a directory's size as 32 bytes per entry, `.` and `..` included; nothing stores it.
        let entries = i64::from(v.nchildren_or_nlink.get()) + 2;
        let links = if expanded {
            v.uncompressed_size.get() as i64 + 2
        } else {
            entries
        };
        ('d', (32 * entries).to_string(), links.to_string(), "-".to_string())
    } else if let Some(target) = &symlink {
        (
            'l',
            target.len().to_string(),
            v.nchildren_or_nlink.get().to_string(),
            escape(target),
        )
    } else {
        let size = match (compression, &e.data) {
            (Some(c), _) if e.inode.is_compressed() => c.size,
            (_, Some(d)) => d.size,
            _ => 0,
        };
        let content = match (sampled(&e.path, hash_every), compression) {
            (false, _) => "-".to_string(),
            (true, Some(c)) if e.inode.is_compressed() => {
                let (dx, ds) = e
                    .xattr(DECMPFS_XATTR_NAME)
                    .map(|(x, s)| (x, s.as_ref()))
                    .unwrap_or_else(|| unreachable!());
                let decmpfs = hex::encode(Sha256::digest(bytes(src, &dx.data, ds)?));
                let fork = match e.xattr(RESOURCE_FORK_XATTR_NAME) {
                    Some((x, s)) => hex::encode(Sha256::digest(bytes(src, &x.data, s.as_ref())?)),
                    None => hex::encode(Sha256::digest(b"")),
                };
                format!("c{}:{decmpfs}:{fork}", c.kind)
            }
            (true, _) => match &e.data {
                Some(d) => sha(src, d)?,
                None => hex::encode(Sha256::digest(b"")),
            },
        };
        let kind = if e.inode.is_regular() { 'f' } else { 'o' };
        (kind, size.to_string(), v.nchildren_or_nlink.get().to_string(), content)
    };
    // listxattr never names a symlink's target (the content column carries it) or an empty resource fork, even with
    // XATTR_SHOWCOMPRESSION.
    let shown = |x: &Xattr, s: &Option<Stream>| match x.name.as_str() {
        SYMLINK_XATTR_NAME => symlink.is_none(),
        RESOURCE_FORK_XATTR_NAME => xattr_len(x, s.as_ref()) != 0,
        _ => true,
    };
    let mut names: Vec<&str> = e
        .xattrs
        .iter()
        .filter(|(x, s)| shown(x, s))
        .map(|(x, _)| x.name.as_str())
        .collect();
    names.sort_unstable();
    let xattrs = if names.is_empty() {
        "-".to_string()
    } else {
        escape(&names.join(","))
    };
    Ok(format!(
        "{}\t{kind}\t{}\t{}:{}\t{:o}\t{size}\t{:#x}\t{nlink}\t{}\t{xattrs}\t{content}",
        escape(&e.path),
        e.inode.oid,
        v.owner.get(),
        v.group.get(),
        v.mode.get() & 0o7777,
        v.bsd_flags.get(),
        v.mod_time.get(),
    ))
}

/// Checks a sealed entry's uncompressed streams against their file-info hashes: SHA-256 over each stretch, the last
/// one zero-padded to whole blocks. A compressed file's resource fork is hashed as its plaintext, so it is skipped.
pub fn check_file_info(src: &dyn BlockSource, e: &Entry) -> Result<usize, ReadError> {
    let mut streams: Vec<&Stream> = e.data.iter().collect();
    for (x, s) in &e.xattrs {
        if let Some(s) = s
            && !(e.inode.is_compressed() && x.name == RESOURCE_FORK_XATTR_NAME)
        {
            streams.push(s);
        }
    }
    let mut checked = 0;
    for s in streams {
        if s.file_info.is_empty() {
            continue;
        }
        let mut data = Vec::new();
        stream::read(src, s, &mut |c| data.extend_from_slice(c))?;
        for info in &s.file_info {
            let start = info.offset as usize;
            let end = start + usize::from(info.hashed_len) * BLOCK_SIZE;
            if info.info_type != APFS_FILE_INFO_DATA_HASH || start > data.len() {
                return Err(ReadError::invalid(format!("{}: file info at {start}", e.path)));
            }
            let mut chunk = data[start..end.min(data.len())].to_vec();
            chunk.resize(end - start, 0);
            if Sha256::digest(&chunk)[..] != info.hash[..] {
                return Err(ReadError::invalid(format!(
                    "{}: stream {} at {start} fails its seal hash",
                    e.path, s.id
                )));
            }
            checked += 1;
        }
    }
    Ok(checked)
}
