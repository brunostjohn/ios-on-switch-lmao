// SPDX-License-Identifier: GPL-3.0-or-later
//! `switch_installd::inject` against `installd.py inject` on the OS volume's installd and the universal and damaged
//! variants `inject_cases.py` makes of it: the same bytes where the Python accepts, a refusal where it refuses.

use std::io::Read;

use sha2::{Digest, Sha256};

struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> &'a [u8] {
        let (head, tail) = self.0.split_at(n);
        self.0 = tail;
        head
    }

    fn field_u16(&mut self) -> &'a [u8] {
        let n = u16::from_le_bytes(self.take(2).try_into().unwrap());
        self.take(n.into())
    }

    fn field_u64(&mut self) -> &'a [u8] {
        let n = u64::from_le_bytes(self.take(8).try_into().unwrap());
        self.take(n as usize)
    }
}

#[test]
fn inject_agrees_with_installd_py() {
    let path = std::env::var("INJECT_CASES").expect("INJECT_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap();
    let mut cases = Reader(&data);
    let (mut accepted, mut refused) = (0, 0);
    while !cases.0.is_empty() {
        let name = String::from_utf8(cases.field_u16().to_vec()).unwrap();
        let packed = cases.field_u64();
        let mut input = Vec::new();
        flate2::read::ZlibDecoder::new(packed).read_to_end(&mut input).unwrap();
        let ours = switch_installd::inject(&input);
        match cases.take(1)[0] {
            1 => {
                let expected = cases.take(32);
                let ours = ours.unwrap_or_else(|e| panic!("{name}: installd.py accepts it, we refuse: {e}"));
                assert_eq!(
                    Sha256::digest(&ours).as_slice(),
                    expected,
                    "{name}: not installd.py's bytes"
                );
                println!("{name}: equal ({} B)", ours.len());
                accepted += 1;
            }
            _ => {
                let message = String::from_utf8_lossy(cases.field_u16()).into_owned();
                let ours = ours
                    .err()
                    .unwrap_or_else(|| panic!("{name}: installd.py refuses it ({message}), we accept"));
                println!("{name}: refused by both: {message} / {ours}");
                refused += 1;
            }
        }
    }
    println!("{accepted} accepted, {refused} refused by both");
    assert!(accepted >= 8 && refused >= 25, "the case file is short");
}
