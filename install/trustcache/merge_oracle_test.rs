// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_trustcache's merge against the build's `trustcache.py` and `trust.py` (merge_oracle.py): the same bytes from
//! merged_module, cdhash_list and the bundle for every set of modules, and a refusal wherever they refuse.

use switch_trustcache::{Entry, HEADER_LEN, bundle, cdhash_list, check_bundle, merged_module, trust_module};

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

    fn expected(&mut self) -> Option<&'a [u8]> {
        if self.take(1)[0] == 1 {
            return None;
        }
        let n = self.u32();
        Some(self.take(n))
    }
}

fn same<E: std::fmt::Display>(case: usize, what: &str, ours: Result<Vec<u8>, E>, oracle: Option<&[u8]>) -> bool {
    match (ours, oracle) {
        (Ok(ours), Some(oracle)) => assert!(ours == oracle, "case {case}: {what} differs"),
        (Err(_), None) => return false,
        (Ok(_), None) => panic!("case {case}: Python refuses {what}, switch_trustcache made it"),
        (Err(e), Some(_)) => panic!("case {case}: {what}: {e}"),
    }
    true
}

#[test]
fn matches_merged_module_cdhash_list_and_bundle() {
    let path = std::env::var("MERGE_ORACLE_CASES").expect("MERGE_ORACLE_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut bytes = Bytes(&data);
    let (mut case, mut made, mut refused) = (0, 0, 0);
    while !bytes.0.is_empty() {
        let modules: Vec<&[u8]> = (0..bytes.u32())
            .map(|_| {
                let n = bytes.u32();
                bytes.take(n)
            })
            .collect();
        let merged = merged_module(&modules);
        let merged_bytes = merged.as_ref().ok().cloned();
        for (what, ours) in [
            ("merged_module", merged),
            ("cdhash_list", cdhash_list(&modules)),
            ("the bundle", merged_bytes.as_deref().map_or(merged_module(&[]), bundle)),
        ] {
            if same(case, what, ours, bytes.expected()) {
                made += 1;
            } else {
                refused += 1;
            }
        }
        if let Some(merged) = merged_bytes {
            if merged.len() > HEADER_LEN {
                check_bundle(&bundle(&merged).unwrap()).unwrap_or_else(|e| panic!("case {case}: {e}"));
            }
        }
        case += 1;
    }
    assert!(
        case >= 13 && made >= 15 && refused >= 15,
        "{case} cases: {made} made, {refused} refused"
    );
    println!("{case} cases: {made} results equal to Python's, {refused} refusals");
}

#[test]
fn check_bundle_refuses_what_the_layout_refuses() {
    let entries = [1u8, 2].map(|b| Entry {
        cdhash: [b; 20],
        flags: 0,
        category: 0,
    });
    let good = bundle(&trust_module(entries, None).unwrap()).unwrap();
    check_bundle(&good).unwrap();
    let mut no_hash_type = good.clone();
    no_hash_type[8 + HEADER_LEN + 20] = 0;
    let mut no_modules = good.clone();
    no_modules[0] = 0;
    let mut misaligned = good.clone();
    misaligned[4] = 9;
    for bad in [
        &no_hash_type[..],
        &no_modules[..],
        &misaligned[..],
        &good[..good.len() - 1],
        &good[..7],
    ] {
        assert!(check_bundle(bad).is_err());
    }
}
