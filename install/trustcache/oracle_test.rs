// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_trustcache against the build's `tools/trustcache/trustcache.py` (oracle.py): the same module for every
//! entry list, and a refusal wherever it refuses.

use switch_trustcache::{CDHASH_LEN, Entry, trust_module};

struct Bytes<'a>(&'a [u8]);

impl<'a> Bytes<'a> {
    fn take(&mut self, n: usize) -> &'a [u8] {
        let (head, rest) = self.0.split_at(n);
        self.0 = rest;
        head
    }

    fn u32(&mut self) -> usize {
        u32::from_be_bytes(self.take(4).try_into().unwrap()) as usize
    }
}

#[test]
fn matches_trust_module() {
    let path = std::env::var("ORACLE_CASES").expect("ORACLE_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut bytes = Bytes(&data);
    let (mut equal, mut refused) = (0, 0);
    while !bytes.0.is_empty() {
        let case = equal + refused;
        let entries: Vec<Entry> = (0..bytes.u32())
            .map(|_| Entry {
                cdhash: bytes.take(CDHASH_LEN).try_into().unwrap(),
                flags: bytes.take(1)[0],
                category: bytes.take(1)[0],
            })
            .collect();
        let has_uuid = bytes.take(1)[0] == 1;
        let uuid: [u8; 16] = bytes.take(16).try_into().unwrap();
        let ours = trust_module(entries, has_uuid.then_some(uuid));
        if bytes.take(1)[0] == 1 {
            assert!(
                ours.is_err(),
                "case {case}: trust_module refuses, switch_trustcache made a module"
            );
            refused += 1;
        } else {
            let n = bytes.u32();
            assert_eq!(
                ours.unwrap_or_else(|e| panic!("case {case}: {e}")),
                bytes.take(n),
                "case {case}"
            );
            equal += 1;
        }
    }
    assert!(
        equal >= 7 && refused >= 2,
        "the oracle wrote {equal} modules and {refused} refusals"
    );
    println!("{equal} modules equal to trust_module, {refused} refusals");
}
