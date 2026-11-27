// SPDX-License-Identifier: GPL-3.0-or-later
//! Each block compressed here against the frame //tools:lz4 (`lz4 -12 -B6 --no-frame-crc`) makes of it
//! (oracle/lz4_cases.py): the same block body, stored or compressed alike.

use switch_window::{compress_blocks, frame_blocks};

struct Bytes<'a>(&'a [u8]);

impl<'a> Bytes<'a> {
    fn take(&mut self) -> &'a [u8] {
        let n = u32::from_be_bytes(self.0[..4].try_into().unwrap()) as usize;
        let (head, rest) = self.0[4..].split_at(n);
        self.0 = rest;
        head
    }
}

#[test]
fn every_block_is_the_cli_frames_block() {
    let path = std::env::var("LZ4_ORACLE_CASES").expect("LZ4_ORACLE_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut bytes = Bytes(&data);
    let (mut blocks, mut frames) = (Vec::new(), Vec::new());
    while !bytes.0.is_empty() {
        blocks.push(bytes.take());
        frames.push(bytes.take());
    }
    let ours = compress_blocks(&blocks).unwrap();
    let mut stored = 0;
    for (i, ((block, frame), ours)) in blocks.iter().zip(&frames).zip(&ours).enumerate() {
        let mut cli = frame_blocks(frame, block.len()).unwrap();
        assert_eq!(cli.len(), 1, "case {i}: the CLI made {} blocks", cli.len());
        let (decoded, cli) = cli.pop().unwrap();
        assert_eq!(decoded, block.len(), "case {i}");
        assert!(
            *ours == cli,
            "case {i} ({} bytes): stored {} here, {} by the CLI",
            block.len(),
            ours.stored,
            cli.stored
        );
        stored += usize::from(cli.stored);
    }
    assert!(
        blocks.len() >= 11 && stored >= 2,
        "{} cases, {stored} stored",
        blocks.len()
    );
    println!("{} blocks equal to lz4 -12's, {stored} of them stored", blocks.len());
}
