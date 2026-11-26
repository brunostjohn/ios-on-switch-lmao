// SPDX-License-Identifier: GPL-3.0-or-later
//! Stand-in content for large new files: bytes from a seed, the same on every call, so a cryptex-sized Preboot image
//! needs no file on the host and its expected SHA-256 can be computed apart from the image.

use std::io::{self, Write};

use switch_apfs_write::Sequential;

const CHUNK: usize = 1 << 20;

/// splitmix64, one word per 8 bytes.
pub fn generate(seed: u64, bytes: u64, out: &mut dyn FnMut(&[u8]) -> io::Result<()>) -> io::Result<()> {
    let mut state = seed;
    let mut buf = vec![0u8; CHUNK];
    let mut left = bytes;
    while left > 0 {
        let n = left.min(CHUNK as u64) as usize;
        for word in buf[..n.next_multiple_of(8)].as_chunks_mut::<8>().0 {
            state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            *word = (z ^ (z >> 31)).to_le_bytes();
        }
        out(&buf[..n])?;
        left -= n as u64;
    }
    Ok(())
}

pub type Generated = Sequential<Box<dyn Fn(&mut dyn FnMut(&[u8]) -> io::Result<()>) -> io::Result<()> + Send + Sync>>;

pub fn fill(seed: u64, bytes: u64) -> Generated {
    Sequential::new(bytes, Box::new(move |out| generate(seed, bytes, out)))
}

pub fn to_stdout(seed: u64, bytes: u64) -> io::Result<()> {
    let stdout = io::stdout();
    let mut out = io::BufWriter::with_capacity(CHUNK, stdout.lock());
    generate(seed, bytes, &mut |b| out.write_all(b))?;
    out.flush()
}
