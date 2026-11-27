// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::{self, ErrorKind, Read, Write};

use flate2::{Decompress, FlushDecompress, Status};
use switch_catalog::Member;

use crate::error::{Error, Result};
use crate::pipe::{Sink, Written, hashed};

const INPUT: usize = 1 << 20;

/// A member's content, from exactly its `stored_size` stored bytes (inflated when the pins say `deflated`), written to
/// `out` as it is made. Refused unless the content's size and SHA-256 are the pinned ones.
pub fn extract<R: Read, W: Write + Send>(stored: R, member: &Member, out: &mut W) -> Result<Written> {
    if member.aea.is_some() {
        return Err(Error::Encrypted(member.member.clone()));
    }
    let mut stored = stored.take(member.stored_size);
    let (consumed, written) = hashed(out, |sink| {
        if member.deflated {
            inflate(&mut stored, sink, &member.member)
        } else {
            copy(&mut stored, sink)
        }
    })?;
    if consumed != member.stored_size {
        return Err(Error::Stored {
            member: member.member.clone(),
            got: consumed,
            expected: member.stored_size,
        });
    }
    if written.len != member.size || written.sha256 != member.content_sha256.0 {
        return Err(Error::Content {
            member: member.member.clone(),
            got_len: written.len,
            got: hex::encode(written.sha256),
            expected_len: member.size,
            expected: member.content_sha256.to_string(),
        });
    }
    Ok(written)
}

pub(crate) fn read_some(source: &mut impl Read, buf: &mut [u8]) -> io::Result<usize> {
    loop {
        match source.read(buf) {
            Err(error) if error.kind() == ErrorKind::Interrupted => {}
            result => return result,
        }
    }
}

pub(crate) fn copy(source: &mut impl Read, sink: &mut Sink) -> Result<u64> {
    let mut total = 0;
    loop {
        let n = sink.fill(|spare| read_some(source, spare).map(|n| (n, n)))?;
        if n == 0 {
            return Ok(total);
        }
        total += n as u64;
    }
}

/// Raw deflate, as zip stores it. The stream must end exactly at the end of the stored bytes.
fn inflate(source: &mut impl Read, sink: &mut Sink, member: &str) -> Result<u64> {
    let bad = |problem: &str| Error::Deflate {
        member: member.to_owned(),
        problem: problem.to_owned(),
    };
    let mut inflater = Decompress::new(false);
    let mut input = vec![0u8; INPUT];
    let (mut start, mut end, mut eof) = (0, 0, false);
    loop {
        if start == end && !eof {
            (start, end) = (0, read_some(source, &mut input)?);
            eof = end == 0;
        }
        let used_before = inflater.total_in();
        let filled = sink.fill(|spare| {
            let made_before = inflater.total_out();
            let status = inflater.decompress(&input[start..end], spare, FlushDecompress::None);
            let status = status.map_err(|error| io::Error::new(ErrorKind::InvalidData, error))?;
            let made = (inflater.total_out() - made_before) as usize;
            Ok((made, (made, status)))
        });
        let (made, status) = filled.map_err(|error| match error.kind() {
            ErrorKind::InvalidData => bad(&error.to_string()),
            _ => error.into(),
        })?;
        let used = (inflater.total_in() - used_before) as usize;
        start += used;
        if status == Status::StreamEnd {
            break;
        }
        if used == 0 && made == 0 {
            return Err(bad(if eof { "ends early" } else { "makes no progress" }));
        }
    }
    if start != end || read_some(source, &mut input[..1])? != 0 {
        return Err(bad("is followed by more bytes"));
    }
    Ok(inflater.total_in())
}
